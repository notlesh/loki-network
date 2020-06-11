#!/usr/bin/env python3
import hive
import pytest

@pytest.fixture(scope="session")
def HiveTenRTenC():
  router_hive = hive.RouterHive(n_relays=10, n_clients=10, netid="hive")
  router_hive.Start()

  yield router_hive

  router_hive.Stop()

@pytest.fixture(scope="session")
def HiveThirtyRTenC():
  router_hive = hive.RouterHive(n_relays=30, n_clients=10, netid="hive")
  router_hive.Start()

  yield router_hive

  router_hive.Stop()

@pytest.fixture()
def HiveArbitrary():
  router_hive = None
  def _make(n_relays=10, n_clients=10, netid="hive"):
    nonlocal router_hive
    router_hive = hive.RouterHive(n_relays=30, n_clients=10, netid="hive")
    router_hive.Start()
    return router_hive

  yield _make

  router_hive.Stop()

@pytest.fixture()
def HiveArbitrary():
  router_hive = None
  # XXX: Jeff:
  # if n_relays is 2 or 3, we get expected connection events (equal in vs. out)
  # by the time we're at 4 relays, we get unexpected, e.g. 385 in, 28538831 out
  def _make(n_relays=4, n_clients=0, netid="hive"):
    nonlocal router_hive
    router_hive = hive.RouterHive(n_relays=30, n_clients=0, netid="hive")
    router_hive.Start()
    return router_hive

  yield _make

  router_hive.Stop()
