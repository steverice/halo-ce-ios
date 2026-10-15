#!/usr/bin/env python3
"""Test native XISO import with synthetic, non-proprietary disc images."""
from pathlib import Path
import hashlib
import os
import random
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/ios/xiso-tests'
BUILD.mkdir(parents=True,exist_ok=True)
PROBE=BUILD/'probe'
subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-g',
    '-fsanitize=address,undefined','-Iport/runtime','port/runtime/xiso.c',
    'port/ios/tests/xiso_probe.c','-o',str(PROBE)],cwd=ROOT,check=True)
NAMES=['ui.map','a10.map','a30.map','a50.map','b30.map','b40.map','c10.map','c20.map','c40.map','d20.map','d40.map']
MAGIC=b'MICROSOFT*XBOX*MEDIA'


def fixture(path, build='01.10.12.2276', names=None, partition=0):
    names=NAMES if names is None else names
    entries=bytearray(2048);payloads={}
    with path.open('wb') as image:
        header=bytearray(2048);header[:20]=header[0x7ec:]=MAGIC
        struct.pack_into('<II',header,20,40,2048)
        image.seek(partition+0x10000);image.write(header)
        root=bytearray(2048);struct.pack_into('<HHIIBB',root,0,0,0,41,2048,16,4);root[14:18]=b'maps'
        image.seek(partition+40*2048);image.write(root)
        for i,name in enumerate(names):
            sector=64+i*2
            data=bytearray(3072);data[:4]=b'daeh';data[2044:2048]=b'toof'
            struct.pack_into('<I',data,4,5);data[64:64+len(build)]=build.encode()
            data[2048:]=bytes([i+1])*1024;payloads[name.lower()]=bytes(data)
            image.seek(partition+sector*2048);image.write(data)
            struct.pack_into('<HHIIBB',entries,i*64,0,(i+1)*16 if i+1<len(names) else 0,sector,len(data),0,len(name))
            entries[i*64+14:i*64+14+len(name)]=name.encode()
        image.seek(partition+41*2048);image.write(entries)
    return payloads


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.image=self.root/'halo.xiso';self.dest=self.root/'out';self.dest.mkdir()
    def tearDown(self):self.temp.cleanup()
    def invoke(self, *args):
        result=subprocess.run([str(PROBE),*map(str,args)],text=True,capture_output=True,
                              env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},timeout=10)
        self.assertNotIn('Sanitizer',result.stderr,result.stderr)
        self.assertNotIn('runtime error:',result.stderr,result.stderr)
        self.assertIn(result.returncode,(0,1),result.stderr)
        return result
    def extract(self, expect=None, cancel=None):
        before=hashlib.sha256(self.image.read_bytes()).digest()
        args=['extract',self.image,self.dest]
        if cancel:args.append(cancel)
        result=self.invoke(*args)
        self.assertEqual(before,hashlib.sha256(self.image.read_bytes()).digest(),'source was changed')
        if expect:
            self.assertEqual(result.returncode,1,result.stdout)
            self.assertIn(expect,result.stdout)
            self.assertFalse((self.dest/'maps').exists())
        else:self.assertEqual(result.returncode,0,result.stdout)
        return result
    def patch(self,offset,data):
        with self.image.open('r+b') as f:f.seek(offset);f.write(data)
    def test_supported_releases_and_exact_bytes(self):
        for build in ('01.10.12.2276','01.01.14.2342'):
            with self.subTest(build=build):
                self.dest=self.root/build;self.dest.mkdir()
                payloads=fixture(self.image,build)
                self.extract()
                for name,data in payloads.items():self.assertEqual(data,(self.dest/'maps'/name).read_bytes())
                self.assertEqual(self.invoke('ready',self.dest/'maps').returncode,0)
    def plan(self, received, total=None):
        result=self.invoke('plan',self.image,received,self.image.stat().st_size if total is None else total)
        return [tuple(map(int,line.split())) for line in result.stdout.splitlines()]
    def test_plan_keeps_only_what_an_import_reads(self):
        """the Apple TV's upload: no plan until the directories have arrived; then the ranges an import reads, and
        an image holding only those (holes elsewhere) imports whole, the image used up as it goes"""
        for partition in (0,0x02080000):
            with self.subTest(partition=partition):
                self.dest=self.root/f'out-{partition}';self.dest.mkdir()
                payloads=fixture(self.image,partition=partition)
                whole=self.image.read_bytes()
                # (the volume descriptor is at sector 32, the root directory at 40, the maps' at 41)
                self.assertEqual(self.plan(partition+0x10000+1024),[])
                self.assertEqual(self.plan(partition+41*2048),[])
                ranges=self.plan(partition+42*2048)
                self.assertTrue(ranges)
                self.assertEqual(ranges,sorted(ranges))
                self.assertTrue(all(a<b and b<=len(whole) for a,b in ranges))
                self.assertTrue(all(ranges[i][1]<ranges[i+1][0] for i in range(len(ranges)-1)),'ranges overlap or touch')
                kept=sum(b-a for a,b in ranges)
                self.assertEqual(kept,2048*3+sum(len(data) for data in payloads.values()))
                # what a receiver that follows the plan has on disk
                sparse=bytearray(len(whole))
                for a,b in ranges:sparse[a:b]=whole[a:b]
                self.image.write_bytes(sparse)
                result=self.invoke('consume',self.image,self.dest)
                self.assertEqual(result.returncode,0,result.stdout)
                for name,data in payloads.items():self.assertEqual(data,(self.dest/'maps'/name).read_bytes())
                self.assertEqual(self.image.stat().st_size,len(whole))
    def test_plan_refuses_what_is_no_disc_image(self):
        self.image.write_bytes(os.urandom(1<<20))
        self.assertEqual(self.plan(1<<20),[])
        fixture(self.image)
        self.patch(41*2048+4,struct.pack('<I',0x7fffffff))    # a map past the end of the image
        self.assertEqual(self.plan(self.image.stat().st_size),[])
    def add_executable(self, data, partition=0):
        """adds default.xbe to the fixture's root directory, after maps"""
        entry=bytearray(28);struct.pack_into('<HHIIBB',entry,0,0,0,200,len(data),0,11);entry[14:25]=b'default.xbe'
        self.patch(partition+40*2048+2,struct.pack('<H',5));self.patch(partition+40*2048+20,bytes(entry))
        self.patch(partition+200*2048,data)
    def test_loading_picture_from_executable(self):
        import sys;sys.path.insert(0,str(ROOT/'tools'));import loading_picture
        # a made-up picture in the executable's form: 320, 240, then runs (count in the high four bits, gray in the low)
        runs=bytes(0xf0|(n*7)%16 for n in range(320*240//15))
        executable=os.urandom(3000)+struct.pack('<II',320,240)+runs+os.urandom(500)
        fixture(self.image);self.add_executable(executable);self.extract()
        pixels=b''.join(bytes([(run&15)<<4])*(run>>4) for run in runs)
        self.assertEqual(loading_picture.find_picture(executable),pixels)
        self.assertEqual((self.dest/'maps'/'loading.tga').read_bytes(),loading_picture.tga(pixels))
        self.assertEqual(self.invoke('ready',self.dest/'maps').returncode,0)
    def test_no_loading_picture(self):
        # an executable without one, and a disc without an executable: the import succeeds, with no picture
        fixture(self.image);self.add_executable(struct.pack('<II',320,240)+bytes(8000));self.extract()
        self.assertFalse((self.dest/'maps'/'loading.tga').exists())
        self.dest=self.root/'bare';self.dest.mkdir();fixture(self.image);self.extract()
        self.assertFalse((self.dest/'maps'/'loading.tga').exists())
    def test_loading_picture_file_not_a_picture(self):
        # maps/loading.tga that is not the 320 by 240 picture is left on the disc, and is no map
        fixture(self.image,names=[*NAMES,'loading.tga']);self.extract()
        self.assertFalse((self.dest/'maps'/'loading.tga').exists())
    def test_whole_disc_partition(self):
        fixture(self.image,partition=0x02080000);self.extract()
    def test_bad_magic(self):
        fixture(self.image);self.patch(0x10000,b'NOPE');self.extract('not an Xbox XISO')
    def test_truncated_extent(self):
        fixture(self.image);self.patch(41*2048+8,struct.pack('<I',0xffffffff));self.extract('truncated')
    def test_missing_campaign(self):
        fixture(self.image,names=NAMES[:-1]);self.extract('d40.map is missing')
    def test_wrong_game_cache(self):
        fixture(self.image);self.patch(64*2048+4,struct.pack('<I',7));self.extract('not an original Xbox Halo map')
    def test_unsupported_build(self):
        fixture(self.image,build='99.99.99.9999');self.extract('Unsupported Xbox map build')
    def test_mixed_releases(self):
        fixture(self.image);self.patch(66*2048+64,b'01.01.14.2342');self.extract('mixed map releases')
    def test_traversal(self):
        fixture(self.image,names=['../ui.map',*NAMES[1:]]);self.extract('unsafe filename')
    def test_duplicate_case(self):
        fixture(self.image,names=['UI.MAP',*NAMES]);self.extract('duplicate filenames')
    def test_cycle(self):
        fixture(self.image);self.patch(41*2048+64+2,struct.pack('<H',16));self.extract('damaged directory tree')
    def test_empty_directory(self):
        fixture(self.image);self.patch(41*2048,struct.pack('<H',0xffff));self.extract('ui.map is missing')
    def test_cancel_and_retry(self):
        fixture(self.image);self.extract('cancelled',cancel=1)
        self.assertEqual(self.invoke('ready',self.dest/'maps.partial').returncode,1)
        self.dest=self.root/'retry';self.dest.mkdir();self.extract()
    def test_existing_files_preserved(self):
        fixture(self.image);maps=self.dest/'maps';maps.mkdir();(maps/'keep').write_text('keep')
        result=self.invoke('extract',self.image,self.dest)
        self.assertEqual(result.returncode,1);self.assertIn('already contains maps',result.stdout)
        self.assertEqual((maps/'keep').read_text(),'keep')
    def test_malformed_directory_mutations(self):
        fixture(self.image);original=self.image.read_bytes();rng=random.Random(27)
        for n in range(60):
            data=bytearray(original)
            offset=41*2048+rng.randrange(len(NAMES))*64+rng.randrange(14)
            data[offset]=rng.randrange(256);self.image.write_bytes(data)
            dest=self.root/f'fuzz-{n}';dest.mkdir();self.invoke('extract',self.image,dest)

if __name__=='__main__':unittest.main()
