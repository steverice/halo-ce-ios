"""Host aim policy and real mouse/controller/touch selection, without assets."""
import re,sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from harness import build,read,function,mutated,run,CHECK_FAILED

def generated(fault=None):
    policy=re.sub(r'^#include "[^"\n]+"\n','',read('port/linux/game/network_aim_assist.c'),flags=re.M)
    input_source=read('port/linux/src/xinput_sdl.c')
    inp=function(input_source,'halo_linux_mouse_aiming')
    version=re.sub(r'^#include "[^"\n]+"\n','',read('port/linux/include/halo_network_version.h'),flags=re.M)
    if fault=='authority': policy=mutated(policy,'machine_index != NONE ||','FALSE ||')
    if fault=='override': inp=mutated(inp,'aim_assist && !network_aim_assist_block_mouse()','aim_assist')
    if fault=='version': version=mutated(version,'return version >= HALO_NETWORK_VERSION_AIM_POLICY;','return 1;')
    return (('policy.inc',policy),('input.inc',inp),('version.inc',version))

@pytest.mark.parametrize('case',['allowed','devices','scope','replication','admission','versions','reset'])
def test_case(case):
    status,output=run(build('aim_assist',generated()),case)
    assert status==0,output

@pytest.mark.parametrize('fault,case',[('authority','admission'),('override','devices'),('version','versions')])
def test_negative(fault,case):
    status,output=run(build('aim_assist',generated(fault)),case)
    assert status==CHECK_FAILED,output
