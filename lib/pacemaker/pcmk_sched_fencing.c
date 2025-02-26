/*
 * Copyright 2004-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <glib.h>

#include <crm/crm.h>
#include <crm/pengine/status.h>
#include <pacemaker-internal.h>
#include "libpacemaker_private.h"

/*!
 * \internal
 * \brief Check whether a resource is known on a particular node
 *
 * \param[in] rsc   Resource to check
 * \param[in] node  Node to check
 *
 * \return TRUE if resource (or parent if an anonymous clone) is known
 */
static bool
rsc_is_known_on(const pcmk_resource_t *rsc, const pcmk_node_t *node)
{
   if (g_hash_table_lookup(rsc->known_on, node->details->id) != NULL) {
       return TRUE;

   } else if ((rsc->variant == pcmk_rsc_variant_primitive)
              && pe_rsc_is_anon_clone(rsc->parent)
              && (g_hash_table_lookup(rsc->parent->known_on,
                                      node->details->id) != NULL)) {
       /* We check only the parent, not the uber-parent, because we cannot
        * assume that the resource is known if it is in an anonymously cloned
        * group (which may be only partially known).
        */
       return TRUE;
   }
   return FALSE;
}

/*!
 * \internal
 * \brief Order a resource's start and promote actions relative to fencing
 *
 * \param[in,out] rsc         Resource to be ordered
 * \param[in,out] stonith_op  Fence action
 */
static void
order_start_vs_fencing(pcmk_resource_t *rsc, pcmk_action_t *stonith_op)
{
    pcmk_node_t *target;

    CRM_CHECK(stonith_op && stonith_op->node, return);
    target = stonith_op->node;

    for (GList *iter = rsc->actions; iter != NULL; iter = iter->next) {
        pcmk_action_t *action = iter->data;

        switch (action->needs) {
            case pcmk_requires_nothing:
                // Anything other than start or promote requires nothing
                break;

            case pcmk_requires_fencing:
                order_actions(stonith_op, action, pcmk__ar_ordered);
                break;

            case pcmk_requires_quorum:
                if (pcmk__str_eq(action->task, PCMK_ACTION_START,
                                 pcmk__str_none)
                    && (g_hash_table_lookup(rsc->allowed_nodes,
                                            target->details->id) != NULL)
                    && !rsc_is_known_on(rsc, target)) {

                    /* If we don't know the status of the resource on the node
                     * we're about to shoot, we have to assume it may be active
                     * there. Order the resource start after the fencing. This
                     * is analogous to waiting for all the probes for a resource
                     * to complete before starting it.
                     *
                     * The most likely explanation is that the DC died and took
                     * its status with it.
                     */
                    pcmk__rsc_debug(rsc, "Ordering %s after %s recovery",
                                    action->uuid, pe__node_name(target));
                    order_actions(stonith_op, action,
                                  pcmk__ar_ordered
                                  |pcmk__ar_unrunnable_first_blocks);
                }
                break;
        }
    }
}

/*!
 * \internal
 * \brief Order a resource's stop and demote actions relative to fencing
 *
 * \param[in,out] rsc         Resource to be ordered
 * \param[in,out] stonith_op  Fence action
 */
static void
order_stop_vs_fencing(pcmk_resource_t *rsc, pcmk_action_t *stonith_op)
{
    GList *iter = NULL;
    GList *action_list = NULL;
    bool order_implicit = false;

    pcmk_resource_t *top = uber_parent(rsc);
    pcmk_action_t *parent_stop = NULL;
    pcmk_node_t *target;

    CRM_CHECK(stonith_op && stonith_op->node, return);
    target = stonith_op->node;

    /* Get a list of stop actions potentially implied by the fencing */
    action_list = pe__resource_actions(rsc, target, PCMK_ACTION_STOP, FALSE);

    /* If resource requires fencing, implicit actions must occur after fencing.
     *
     * Implied stops and demotes of resources running on guest nodes are always
     * ordered after fencing, even if the resource does not require fencing,
     * because guest node "fencing" is actually just a resource stop.
     */
    if (pcmk_is_set(rsc->flags, pcmk_rsc_needs_fencing)
        || pe__is_guest_node(target)) {

        order_implicit = true;
    }

    if (action_list && order_implicit) {
        parent_stop = find_first_action(top->actions, NULL, PCMK_ACTION_STOP,
                                        NULL);
    }

    for (iter = action_list; iter != NULL; iter = iter->next) {
        pcmk_action_t *action = iter->data;

        // The stop would never complete, so convert it into a pseudo-action.
        pe__set_action_flags(action, pcmk_action_pseudo|pcmk_action_runnable);

        if (order_implicit) {
            /* Order the stonith before the parent stop (if any).
             *
             * Also order the stonith before the resource stop, unless the
             * resource is inside a bundle -- that would cause a graph loop.
             * We can rely on the parent stop's ordering instead.
             *
             * User constraints must not order a resource in a guest node
             * relative to the guest node container resource. The
             * pcmk__ar_guest_allowed flag marks constraints as generated by the
             * cluster and thus immune to that check (and is irrelevant if
             * target is not a guest).
             */
            if (!pe_rsc_is_bundled(rsc)) {
                order_actions(stonith_op, action, pcmk__ar_guest_allowed);
            }
            order_actions(stonith_op, parent_stop, pcmk__ar_guest_allowed);
        }

        if (pcmk_is_set(rsc->flags, pcmk_rsc_failed)) {
            crm_notice("Stop of failed resource %s is implicit %s %s is fenced",
                       rsc->id, (order_implicit? "after" : "because"),
                       pe__node_name(target));
        } else {
            crm_info("%s is implicit %s %s is fenced",
                     action->uuid, (order_implicit? "after" : "because"),
                     pe__node_name(target));
        }

        if (pcmk_is_set(rsc->flags, pcmk_rsc_notify)) {
            pe__order_notifs_after_fencing(action, rsc, stonith_op);
        }

#if 0
        /* It might be a good idea to stop healthy resources on a node about to
         * be fenced, when possible.
         *
         * However, fencing must be done before a failed resource's
         * (pseudo-)stop action, so that could create a loop. For example, given
         * a group of A and B running on node N with a failed stop of B:
         *
         *    fence N -> stop B (pseudo-op) -> stop A -> fence N
         *
         * The block below creates the stop A -> fence N ordering and therefore
         * must (at least for now) be disabled. Instead, run the block above and
         * treat all resources on N as B would be (i.e., as a pseudo-op after
         * the fencing).
         *
         * @TODO Maybe break the "A requires B" dependency in
         * pcmk__update_action_for_orderings() and use this block for healthy
         * resources instead of the above.
         */
         crm_info("Moving healthy resource %s off %s before fencing",
                  rsc->id, pe__node_name(node));
         pcmk__new_ordering(rsc, stop_key(rsc), NULL, NULL,
                            strdup(PCMK_ACTION_STONITH), stonith_op,
                            pcmk__ar_ordered, rsc->cluster);
#endif
    }

    g_list_free(action_list);

    /* Get a list of demote actions potentially implied by the fencing */
    action_list = pe__resource_actions(rsc, target, PCMK_ACTION_DEMOTE, FALSE);

    for (iter = action_list; iter != NULL; iter = iter->next) {
        pcmk_action_t *action = iter->data;

        if (!(action->node->details->online) || action->node->details->unclean
            || pcmk_is_set(rsc->flags, pcmk_rsc_failed)) {

            if (pcmk_is_set(rsc->flags, pcmk_rsc_failed)) {
                pcmk__rsc_info(rsc,
                               "Demote of failed resource %s is implicit "
                               "after %s is fenced",
                               rsc->id, pe__node_name(target));
            } else {
                pcmk__rsc_info(rsc, "%s is implicit after %s is fenced",
                               action->uuid, pe__node_name(target));
            }

            /* The demote would never complete and is now implied by the
             * fencing, so convert it into a pseudo-action.
             */
            pe__set_action_flags(action,
                                 pcmk_action_pseudo|pcmk_action_runnable);

            if (pe_rsc_is_bundled(rsc)) {
                // Recovery will be ordered as usual after parent's implied stop

            } else if (order_implicit) {
                order_actions(stonith_op, action,
                              pcmk__ar_guest_allowed|pcmk__ar_ordered);
            }
        }
    }

    g_list_free(action_list);
}

/*!
 * \internal
 * \brief Order resource actions properly relative to fencing
 *
 * \param[in,out] rsc         Resource whose actions should be ordered
 * \param[in,out] stonith_op  Fencing operation to be ordered against
 */
static void
rsc_stonith_ordering(pcmk_resource_t *rsc, pcmk_action_t *stonith_op)
{
    if (rsc->children) {
        for (GList *iter = rsc->children; iter != NULL; iter = iter->next) {
            pcmk_resource_t *child_rsc = iter->data;

            rsc_stonith_ordering(child_rsc, stonith_op);
        }

    } else if (!pcmk_is_set(rsc->flags, pcmk_rsc_managed)) {
        pcmk__rsc_trace(rsc,
                        "Skipping fencing constraints for unmanaged resource: "
                        "%s", rsc->id);

    } else {
        order_start_vs_fencing(rsc, stonith_op);
        order_stop_vs_fencing(rsc, stonith_op);
    }
}

/*!
 * \internal
 * \brief Order all actions appropriately relative to a fencing operation
 *
 * Ensure start operations of affected resources are ordered after fencing,
 * imply stop and demote operations of affected resources by marking them as
 * pseudo-actions, etc.
 *
 * \param[in,out] stonith_op  Fencing operation
 * \param[in,out] scheduler   Scheduler data
 */
void
pcmk__order_vs_fence(pcmk_action_t *stonith_op, pcmk_scheduler_t *scheduler)
{
    CRM_CHECK(stonith_op && scheduler, return);
    for (GList *r = scheduler->resources; r != NULL; r = r->next) {
        rsc_stonith_ordering((pcmk_resource_t *) r->data, stonith_op);
    }
}

/*!
 * \internal
 * \brief Order an action after unfencing
 *
 * \param[in]     rsc       Resource that action is for
 * \param[in,out] node      Node that action is on
 * \param[in,out] action    Action to be ordered after unfencing
 * \param[in]     order     Ordering flags
 */
void
pcmk__order_vs_unfence(const pcmk_resource_t *rsc, pcmk_node_t *node,
                       pcmk_action_t *action,
                       enum pcmk__action_relation_flags order)
{
    /* When unfencing is in use, we order unfence actions before any probe or
     * start of resources that require unfencing, and also of fence devices.
     *
     * This might seem to violate the principle that fence devices require
     * only quorum. However, fence agents that unfence often don't have enough
     * information to even probe or start unless the node is first unfenced.
     */
    if ((pcmk_is_set(rsc->flags, pcmk_rsc_fence_device)
         && pcmk_is_set(rsc->cluster->flags, pcmk_sched_enable_unfencing))
        || pcmk_is_set(rsc->flags, pcmk_rsc_needs_unfencing)) {

        /* Start with an optional ordering. Requiring unfencing would result in
         * the node being unfenced, and all its resources being stopped,
         * whenever a new resource is added -- which would be highly suboptimal.
         */
        pcmk_action_t *unfence = pe_fence_op(node, PCMK_ACTION_ON, TRUE, NULL,
                                           FALSE, node->details->data_set);

        order_actions(unfence, action, order);

        if (!pcmk__node_unfenced(node)) {
            // But unfencing is required if it has never been done
            char *reason = crm_strdup_printf("required by %s %s",
                                             rsc->id, action->task);

            trigger_unfencing(NULL, node, reason, NULL,
                              node->details->data_set);
            free(reason);
        }
    }
}

/*!
 * \internal
 * \brief Create pseudo-op for guest node fence, and order relative to it
 *
 * \param[in,out] node  Guest node to fence
 */
void
pcmk__fence_guest(pcmk_node_t *node)
{
    pcmk_resource_t *container = NULL;
    pcmk_action_t *stop = NULL;
    pcmk_action_t *stonith_op = NULL;

    /* The fence action is just a label; we don't do anything differently for
     * off vs. reboot. We specify it explicitly, rather than let it default to
     * cluster's default action, because we are not _initiating_ fencing -- we
     * are creating a pseudo-event to describe fencing that is already occurring
     * by other means (container recovery).
     */
    const char *fence_action = PCMK_ACTION_OFF;

    CRM_ASSERT(node != NULL);

    /* Check whether guest's container resource has any explicit stop or
     * start (the stop may be implied by fencing of the guest's host).
     */
    container = node->details->remote_rsc->container;
    if (container) {
        stop = find_first_action(container->actions, NULL, PCMK_ACTION_STOP,
                                 NULL);

        if (find_first_action(container->actions, NULL, PCMK_ACTION_START,
                              NULL)) {
            fence_action = PCMK_ACTION_REBOOT;
        }
    }

    /* Create a fence pseudo-event, so we have an event to order actions
     * against, and the controller can always detect it.
     */
    stonith_op = pe_fence_op(node, fence_action, FALSE, "guest is unclean",
                             FALSE, node->details->data_set);
    pe__set_action_flags(stonith_op, pcmk_action_pseudo|pcmk_action_runnable);

    /* We want to imply stops/demotes after the guest is stopped, not wait until
     * it is restarted, so we always order pseudo-fencing after stop, not start
     * (even though start might be closer to what is done for a real reboot).
     */
    if ((stop != NULL) && pcmk_is_set(stop->flags, pcmk_action_pseudo)) {
        pcmk_action_t *parent_stonith_op = pe_fence_op(stop->node, NULL, FALSE,
                                                     NULL, FALSE,
                                                     node->details->data_set);

        crm_info("Implying guest %s is down (action %d) after %s fencing",
                 pe__node_name(node), stonith_op->id,
                 pe__node_name(stop->node));
        order_actions(parent_stonith_op, stonith_op,
                      pcmk__ar_unrunnable_first_blocks
                      |pcmk__ar_first_implies_then);

    } else if (stop) {
        order_actions(stop, stonith_op,
                      pcmk__ar_unrunnable_first_blocks
                      |pcmk__ar_first_implies_then);
        crm_info("Implying guest %s is down (action %d) "
                 "after container %s is stopped (action %d)",
                 pe__node_name(node), stonith_op->id,
                 container->id, stop->id);
    } else {
        /* If we're fencing the guest node but there's no stop for the guest
         * resource, we must think the guest is already stopped. However, we may
         * think so because its resource history was just cleaned. To avoid
         * unnecessarily considering the guest node down if it's really up,
         * order the pseudo-fencing after any stop of the connection resource,
         * which will be ordered after any container (re-)probe.
         */
        stop = find_first_action(node->details->remote_rsc->actions, NULL,
                                 PCMK_ACTION_STOP, NULL);

        if (stop) {
            order_actions(stop, stonith_op, pcmk__ar_ordered);
            crm_info("Implying guest %s is down (action %d) "
                     "after connection is stopped (action %d)",
                     pe__node_name(node), stonith_op->id, stop->id);
        } else {
            /* Not sure why we're fencing, but everything must already be
             * cleanly stopped.
             */
            crm_info("Implying guest %s is down (action %d) ",
                     pe__node_name(node), stonith_op->id);
        }
    }

    // Order/imply other actions relative to pseudo-fence as with real fence
    pcmk__order_vs_fence(stonith_op, node->details->data_set);
}

/*!
 * \internal
 * \brief Check whether node has already been unfenced
 *
 * \param[in] node  Node to check
 *
 * \return true if node has a nonzero #node-unfenced attribute (or none),
 *         otherwise false
 */
bool
pcmk__node_unfenced(const pcmk_node_t *node)
{
    const char *unfenced = pe_node_attribute_raw(node, CRM_ATTR_UNFENCED);

    return !pcmk__str_eq(unfenced, "0", pcmk__str_null_matches);
}

/*!
 * \internal
 * \brief Order a resource's start and stop relative to unfencing of a node
 *
 * \param[in,out] data       Node that could be unfenced
 * \param[in,out] user_data  Resource to order
 */
void
pcmk__order_restart_vs_unfence(gpointer data, gpointer user_data)
{
    pcmk_node_t *node = (pcmk_node_t *) data;
    pcmk_resource_t *rsc = (pcmk_resource_t *) user_data;

    pcmk_action_t *unfence = pe_fence_op(node, PCMK_ACTION_ON, true, NULL,
                                         false, rsc->cluster);

    crm_debug("Ordering any stops of %s before %s, and any starts after",
              rsc->id, unfence->uuid);

    /*
     * It would be more efficient to order clone resources once,
     * rather than order each instance, but ordering the instance
     * allows us to avoid unnecessary dependencies that might conflict
     * with user constraints.
     *
     * @TODO: This constraint can still produce a transition loop if the
     * resource has a stop scheduled on the node being unfenced, and
     * there is a user ordering constraint to start some other resource
     * (which will be ordered after the unfence) before stopping this
     * resource. An example is "start some slow-starting cloned service
     * before stopping an associated virtual IP that may be moving to
     * it":
     *       stop this -> unfencing -> start that -> stop this
     */
    pcmk__new_ordering(rsc, stop_key(rsc), NULL,
                       NULL, strdup(unfence->uuid), unfence,
                       pcmk__ar_ordered|pcmk__ar_if_on_same_node,
                       rsc->cluster);

    pcmk__new_ordering(NULL, strdup(unfence->uuid), unfence,
                       rsc, start_key(rsc), NULL,
                       pcmk__ar_first_implies_same_node_then
                       |pcmk__ar_if_on_same_node,
                       rsc->cluster);
}
