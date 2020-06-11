import pyllarp
from time import time

def test_peer_stats(HiveArbitrary):
  hive = HiveArbitrary(n_relays=2, n_clients=0)

  start_time = time()
  cur_time = start_time
  test_duration = 120 #seconds

  paths = []

  print("looking for events...")

  numInbound = 0
  numOutbound = 0

  while cur_time < start_time + test_duration:

    hive.CollectAllEvents()

    for event in hive.events:
      event_name = event.__class__.__name__

    if event_name == "LinkSessionEstablishedEvent":
      '''
      print("{} session established ({} -> {})",
          ("inbound" if event.inbound else "outbound"), 
          event.routerID,
          event.remoteId)
          '''

      if event.inbound:
        numInbound += 1
      else:
        numOutbound += 1

    hive.events = []
    cur_time = time()

  print("test duration exceeded")
  assert(numInbound == numOutbound)

if __name__ == "__main__":
  main()
