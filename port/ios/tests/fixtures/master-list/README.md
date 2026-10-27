# Master response captures

These are public HTTPS response bodies from the official OpenCE master at
`https://master.opencommunityedition.com/v1/servers`, captured without reading
OpenCE source or its test data. They contain protocol metadata and signed
listings, with no game assets.

- `official-empty.json`: October 25, 2026; the matching official master signature
  is preserved in `master_list_probe.c`. Its signed time is 1792961972.
- `official-populated.json`: October 27, 2026; 23 listings, including 22 dedicated
  servers. Its signed time is 1793142772. Each listing retains its host signature.
  The capture did not include a master signature header, so tests use it only
  after the private authenticated-body seam.

Tests supply the capture timestamps explicitly and do not contact the master.
