# Quest collect-item counts

Quest itemCounts previously depended on SMSG_QUESTUPDATE_ADD_ITEM deltas. Items already present before login/accept/query remained zero; removed items could leave stale counts, and a bag refresh followed by a loot notification could double count.

The session now reconciles every known collect-item objective from Inventory::countItem after an inventory rebuild (before bag UI events), and after quest query metadata arrives. Counts cover backpack and equipped-bag contents, clamp to the objective requirement, and decrease when items are sold, destroyed, consumed or removed. Moving/splitting stacks does not add progress. Add-item notifications keep their chat/sound behavior but no longer add a delta to an already reconciled count.

The existing tracker cache reads the updated session counts within its half-second refresh, and the quest log shares them. Quest completion remains server-authoritative. No asset rebuild is needed.
