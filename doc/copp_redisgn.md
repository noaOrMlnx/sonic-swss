# CoPP Manager Redesign HLD

## Overview

In the current design of Copp Manager, before installing a trap, CoppMgr checks if one of the following requirements is fulfilled: 

1. Trap name is in the features table and enabled.
2. Trap name does not exist in the features table. (e.g. arp, lacp, udld, ip2me).

If both requirments above do not exist, the trap will not be installed.

An issue in the logic was exposed, when for sflow feature, the entry was deleted from the features table, but the trap was anyway installed. This case is not expected since we can't know if the feature is enabled or disabled.


The fix for the above issue will require a change in logic:

* If the feature is disabled or does not have an entry in te features table, don't install the trap.
 
* If the trap name does not exist in the features table, but the trap has a field which called "always_enabled" and it's value is "true", install the trap.

* If there is a feature in the features table that is enabled, install the associated trap.

* If there is a feature which is in state 'disabled', but the associated trap has "always_enabled": "true" field, install the trap.

In this way, we can avoid the unknown situation when the feature entry is deleted, but install the traps which have no feature entry.

NOTE: If a trap has "always_enabled":"true" field, we don't check the features table.

## Requirements

* A new field will be added to copp_cfg.j2 template called "always_enabled".

    arp, ip2me, udld and lacp traps will have "always_enabled" : "true" field.

    All of the rest traps won't need to have the new field. by default it's value will be false.

* The new COPP_TRAP table will use the following format:

    ```
    "COPP_TRAP": {
        "bgp": {
            "trap_ids": "bgp,bgpv6",
            "trap_group": "queue4_group1"
        },
        "arp": {
            "trap_ids": "arp_req,arp_resp,neigh_discovery",
            "trap_group": "queue4_group2",
            "always_enabled": "true"
        },
        "udld": {
		    "trap_ids": "udld",
		    "trap_group": "queue4_group3",
             "always_enabled": "true"
	    },
	    "ip2me": {
		    "trap_ids": "ip2me",
		    "trap_group": "queue1_group1",
             "always_enabled": "true"
	    },
        "lacp": {
		    "trap_ids": "lacp",
		    "trap_group": "queue4_group1",
             "always_enabled": "true"
	    }
    }
    ```

* The features table:

```
admin@sonic:~$ show feature status
Feature         State           AutoRestart     SetOwner
--------------  --------------  --------------  ----------
bgp             enabled         enabled
database        always_enabled  always_enabled
dhcp_relay      enabled         enabled         local
lldp            enabled         enabled
macsec          disabled        enabled
mgmt-framework  enabled         enabled
nat             disabled        enabled
pmon            enabled         enabled
radv            enabled         enabled
sflow           disabled        enabled
snmp            enabled         enabled
swss            enabled         enabled
syncd           enabled         enabled
teamd           enabled         enabled
telemetry       enabled         enabled

```

* Copp Manager does not have a stand alone CLI, so a new CLI for changing the "always_enabled" field is not required.

* The traps are not saved to config_db.json, so there is no need of a DB migrator.

* The YANG model will need to be changed in order to accumulate the CoPP change.

## Implementation

The implementation will mainly change the logic of Copp Manager.

* In the constructor on CoppMgr, we will check if there is a trap which does not have an entry in features table, and also does not have "always_enabled":"true" field.

If so, the trap will be added to "m_coppDisabledTraps" list, which means it will not be installed.

* If there is a trap with "always_enabled":"true" field, we will install it (don't check the features table).


* In doCoppTrapTask, a check for the new field will be added.

    * If there is a trap that changed the "always_enabled" value from false to true, the trap will be installed.

    * If there is a trap that changed the "always_enabled" value from true to false, and there is not an entry in the features table, or if the feature is disabled - the trap will be removed.

    * If a new trap was added, it will be handeled according to it's fields.


* in doFeatureTask function:
    * If the OP is DEL - deleting a line in features table, and the feature has the "always_enabled": "true" field in the associated trap, don't remove the trap installation.

    * If one of the features state changed to be "enabled", or a new entry was added, and there is an associated trap which has the "always_enabled": "true" field, don't install the trap - it is already install.

## Unit tests

There is already a Copp unit test - src/sonic-swss/tests/test_copp.py that will need to be changed.

A map called "copp_trap" will be changed and will contain the name of the trap and a list of 3 values: 
trap_id, trap_group, is_always_enabled.

#### Initial map:

```
copp_trap = {
        "bgp,bgpv6": copp_group_queue4_group1,
        "lacp": copp_group_queue4_group1,
        "arp_req,arp_resp,neigh_discovery":copp_group_queue4_group2,
        "lldp":copp_group_queue4_group3,
        "dhcp,dhcpv6":copp_group_queue4_group3,
        "udld":copp_group_queue4_group3,
        "ip2me":copp_group_queue1_group1,
        "src_nat_miss,dest_nat_miss": copp_group_queue1_group2,
        "sample_packet": copp_group_queue2_group1,
        "ttl_error": copp_group_default
}
```

#### Changed map:

```
copp_trap = {
        "bgp": ["bgp;bgpv6", copp_group_queue4_group1],
        "lacp": ["lacp", copp_group_queue4_group1, "always_enabled"],
        "arp": ["arp_req;arp_resp;neigh_discovery", copp_group_queue4_group2, "always_enabled"],
        "lldp": ["lldp", copp_group_queue4_group3],
        "dhcp": ["dhcp;dhcpv6", copp_group_queue4_group3],
        "udld": ["udld", copp_group_queue4_group3, "always_enabled"],
        "ip2me": ["ip2me", copp_group_queue1_group1, "always_enabled"],
        "nat": ["src_nat_miss,dest_nat_miss": copp_group_queue1_group2],
        "sflow": ["sample_packet": copp_group_queue2_group1],
        "ttl": ["ttl_error": copp_group_default]
}
```

### Tests to be changed

* All tests will use the copp_trap map according to it's new defition.

* "test_defaults" will check that the expected traps have the always_enabled value as "True".

* Any assignment of trap_ctbl will now be changed and contain the new "always_enabled" field. 


### New tests

* "test_trap_always_enabled_add" will be added. The test will add a new trap, which has no entry in the features table, and the "always_enabled" field value will be true.
The test will expect the new trap to be installed.

* "test_always_enabled_set_to_false" will be added. The test will check 2 cases:

    * If there is an entry in the features table and it's enabled, the trap should still be installed.

    * If the feature is disabled or if there is not an entry in the features table, the test will expect the trap to be removed.
