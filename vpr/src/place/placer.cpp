
#include "placer.h"

#include "vtr_time.h"
#include "read_place.h"
#include "analytic_placer.h"
#include "initial_placement.h"
#include "concrete_timing_info.h"
#include "tatum/echo_writer.hpp"
#include "verify_placement.h"
#include "place_timing_update.h"

Placer::Placer(const Netlist<>& net_list,
               const t_placer_opts& placer_opts,
               const t_analysis_opts& analysis_opts,
               const t_noc_opts& noc_opts,
               const std::vector<t_direct_inf>& directs,
               std::shared_ptr<PlaceDelayModel> place_delay_model,
               bool cube_bb)
    : placer_opts_(placer_opts)
    , noc_opts_(noc_opts)
    , costs_(placer_opts.place_algorithm, noc_opts.noc)
    , placer_state_(placer_opts.place_algorithm.is_timing_driven(), cube_bb)
    , rng_(placer_opts.seed)
    , net_cost_handler_(placer_opts, placer_state_, cube_bb)
    , place_delay_model_(place_delay_model){
    const auto& cluster_ctx = g_vpr_ctx.clustering();
    const auto& device_ctx = g_vpr_ctx.device();
    const auto& atom_ctx = g_vpr_ctx.atom();

    init_placement_context(placer_state_.mutable_blk_loc_registry(), directs);

    // create a NoC cost handler if NoC optimization is enabled
    if (noc_opts.noc) {
        noc_cost_handler_.emplace(placer_state_.block_locs());
    }

    // Start measuring placement time
    timer_ = std::make_unique<vtr::ScopedStartFinishTimer>("Placement");

    /* To make sure the importance of NoC-related cost terms compared to
     * BB and timing cost is determine only through NoC placement weighting factor,
     * we normalize NoC-related cost weighting factors so that they add up to 1.
     * With this normalization, NoC-related cost weighting factors only determine
     * the relative importance of NoC cost terms with respect to each other, while
     * the importance of total NoC cost to conventional placement cost is determined
     * by NoC placement weighting factor.
     */
    if (noc_opts.noc) {
        normalize_noc_cost_weighting_factor(const_cast<t_noc_opts&>(noc_opts));
    }


    BlkLocRegistry& blk_loc_registry = placer_state_.mutable_blk_loc_registry();
    initial_placement(placer_opts, placer_opts.constraints_file.c_str(),
                      noc_opts, blk_loc_registry, noc_cost_handler_, rng_);

    //create the move generator based on the chosen placement strategy
//    auto [move_generator, move_generator2] = create_move_generators(placer_state_, placer_opts, move_lim, noc_opts.noc_centroid_weight, rng_);

    if (!placer_opts.write_initial_place_file.empty()) {
        print_place(nullptr, nullptr, placer_opts.write_initial_place_file.c_str(), placer_state_.block_locs());
    }

#ifdef ENABLE_ANALYTIC_PLACE
    /*
     * Analytic Placer:
     *  Passes in the initial_placement via vpr_context, and passes its placement back via locations marked on
     *  both the clb_netlist and the gird.
     *  Most of anneal is disabled later by setting initial temperature to 0 and only further optimizes in quench
     */
    if (placer_opts.enable_analytic_placer) {
        AnalyticPlacer{blk_loc_registry}.ap_place();
    }

#endif /* ENABLE_ANALYTIC_PLACE */

    // Update physical pin values
   for (const ClusterBlockId block_id : cluster_ctx.clb_nlist.blocks()) {
       blk_loc_registry.place_sync_external_block_connections(block_id);
   }

   // Allocate here because it goes into timing critical code where each memory allocation is expensive
   pb_gpin_lookup_ = IntraLbPbPinLookup(device_ctx.logical_block_types);
   // Enables fast look-up of atom pins connect to CLB pins
   netlist_pin_lookup_ = ClusteredPinAtomPinsLookup(cluster_ctx.clb_nlist, atom_ctx.nlist, pb_gpin_lookup_);

   // Gets initial cost and loads bounding boxes.
   costs_.bb_cost = net_cost_handler_.comp_bb_cost(e_cost_methods::NORMAL);
   costs_.bb_cost_norm = 1 / costs_.bb_cost;

   if (placer_opts.place_algorithm.is_timing_driven()) {
       alloc_and_init_timing_objects_(net_list, analysis_opts);
   } else {
       VTR_ASSERT(placer_opts.place_algorithm == e_place_algorithm::BOUNDING_BOX_PLACE);
       // Timing cost and normalization factors are not used
       constexpr double INVALID_COST = std::numeric_limits<double>::quiet_NaN();
       costs_.timing_cost = INVALID_COST;
       costs_.timing_cost_norm = INVALID_COST;
   }

   if (noc_opts.noc) {
       VTR_ASSERT(noc_cost_handler_.has_value());

       // get the costs associated with the NoC
       costs_.noc_cost_terms.aggregate_bandwidth = noc_cost_handler_->comp_noc_aggregate_bandwidth_cost();
       std::tie(costs_.noc_cost_terms.latency, costs_.noc_cost_terms.latency_overrun) = noc_cost_handler_->comp_noc_latency_cost();
       costs_.noc_cost_terms.congestion = noc_cost_handler_->comp_noc_congestion_cost();

       // initialize all the noc normalization factors
       noc_cost_handler_->update_noc_normalization_factors(costs_);
   }

   // set the starting total placement cost
   costs_.cost = costs_.get_total_cost(placer_opts, noc_opts);
}

void Placer::alloc_and_init_timing_objects_(const Netlist<>& net_list,
                                            const t_analysis_opts& analysis_opts) {
   const auto& atom_ctx = g_vpr_ctx.atom();
   const auto& cluster_ctx = g_vpr_ctx.clustering();
   const auto& timing_ctx = g_vpr_ctx.timing();
   const auto& p_timing_ctx = placer_state_.timing();

   // Update the point-to-point delays from the initial placement
   comp_td_connection_delays(place_delay_model_.get(), placer_state_);

   // Initialize timing analysis
   placement_delay_calc_ = std::make_shared<PlacementDelayCalculator>(atom_ctx.nlist,
                                                                      atom_ctx.lookup,
                                                                      p_timing_ctx.connection_delay,
                                                                      /*is_flat=*/false);
   placement_delay_calc_->set_tsu_margin_relative(placer_opts_.tsu_rel_margin);
   placement_delay_calc_->set_tsu_margin_absolute(placer_opts_.tsu_abs_margin);

   timing_info_ = make_setup_timing_info(placement_delay_calc_, placer_opts_.timing_update_type);

   placer_setup_slacks_ = std::make_unique<PlacerSetupSlacks>(cluster_ctx.clb_nlist, netlist_pin_lookup_);

   placer_criticalities_ = std::make_unique<PlacerCriticalities>(cluster_ctx.clb_nlist, netlist_pin_lookup_);

   pin_timing_invalidator_ = make_net_pin_timing_invalidator(placer_opts_.timing_update_type,
                                                             net_list,
                                                             netlist_pin_lookup_,
                                                             atom_ctx.nlist,
                                                             atom_ctx.lookup,
                                                             *timing_info_->timing_graph(),
                                                             /*is_flat=*/false);

   // First time compute timing and costs, compute from scratch
   PlaceCritParams crit_params;
   crit_params.crit_exponent = placer_opts_.td_place_exp_first;
   crit_params.crit_limit = placer_opts_.place_crit_limit;

   initialize_timing_info(crit_params, place_delay_model_.get(), placer_criticalities_.get(),
                          placer_setup_slacks_.get(), pin_timing_invalidator_.get(),
                          timing_info_.get(), &costs_, placer_state_);

   critical_path_ = timing_info_->least_slack_critical_path();

   // Write out the initial timing echo file
   if (isEchoFileEnabled(E_ECHO_INITIAL_PLACEMENT_TIMING_GRAPH)) {
       tatum::write_echo(
           getEchoFileName(E_ECHO_INITIAL_PLACEMENT_TIMING_GRAPH),
           *timing_ctx.graph, *timing_ctx.constraints,
           *placement_delay_calc_, timing_info_->analyzer());

       tatum::NodeId debug_tnode = id_or_pin_name_to_tnode(analysis_opts.echo_dot_timing_graph_node);

       write_setup_timing_graph_dot(
           getEchoFileName(E_ECHO_INITIAL_PLACEMENT_TIMING_GRAPH)
               + std::string(".dot"),
           *timing_info_, debug_tnode);
   }

   costs_.timing_cost_norm = 1 / costs_.timing_cost;

   // Sanity check that initial placement is legal
   check_place_();

   print_initial_placement_stats_();

#ifndef ENABLE_ANALYTIC_PLACE
   annealer_ = std::make_unique(placer_opts, placer_state, costs, net_cost_handler, noc_cost_handler,
                                noc_opts, rng, std::move(move_generator), std::move(move_generator2), place_delay_model.get(),
                                placer_criticalities.get(), placer_setup_slacks.get(), timing_info.get(), pin_timing_invalidator.get(),
                                move_lim);
#endif
}

void Placer::check_place_() {
   const ClusteredNetlist& clb_nlist = g_vpr_ctx.clustering().clb_nlist;
   const DeviceGrid& device_grid = g_vpr_ctx.device().grid;
   const auto& cluster_constraints = g_vpr_ctx.floorplanning().cluster_constraints;

   int error = 0;

   // Verify the placement invariants independent to the placement flow.
   error += verify_placement(placer_state_.blk_loc_registry(),
                             clb_nlist,
                             device_grid,
                             cluster_constraints);

   error += check_placement_costs_();

   if (noc_opts_.noc) {
       // check the NoC costs during placement if the user is using the NoC supported flow
       error += noc_cost_handler_->check_noc_placement_costs(costs_, PL_INCREMENTAL_COST_TOLERANCE, noc_opts_);
       // make sure NoC routing configuration does not create any cycles in CDG
       error += (int)noc_cost_handler_->noc_routing_has_cycle();
   }

   if (error == 0) {
       VTR_LOG("\n");
       VTR_LOG("Completed placement consistency check successfully.\n");

   } else {
       VPR_ERROR(VPR_ERROR_PLACE,
                 "\nCompleted placement consistency check, %d errors found.\n"
                 "Aborting program.\n",
                 error);
   }
}

int Placer::check_placement_costs_() {
   int error = 0;
   double timing_cost_check;

   double bb_cost_check = net_cost_handler_.comp_bb_cost(e_cost_methods::CHECK);

   if (fabs(bb_cost_check - costs_.bb_cost) > costs_.bb_cost * PL_INCREMENTAL_COST_TOLERANCE) {
       VTR_LOG_ERROR(
           "bb_cost_check: %g and bb_cost: %g differ in check_place.\n",
           bb_cost_check, costs_.bb_cost);
       error++;
   }

   if (placer_opts_.place_algorithm.is_timing_driven()) {
       comp_td_costs(place_delay_model_.get(), *placer_criticalities_, placer_state_, &timing_cost_check);
       //VTR_LOG("timing_cost recomputed from scratch: %g\n", timing_cost_check);
       if (fabs(timing_cost_check - costs_.timing_cost) > costs_.timing_cost * PL_INCREMENTAL_COST_TOLERANCE) {
           VTR_LOG_ERROR(
               "timing_cost_check: %g and timing_cost: %g differ in check_place.\n",
               timing_cost_check, costs_.timing_cost);
           error++;
       }
   }
   return error;
}

void Placer::print_initial_placement_stats_() {
   VTR_LOG("Initial placement cost: %g bb_cost: %g td_cost: %g\n",
           costs_.cost, costs_.bb_cost, costs_.timing_cost);

   if (noc_opts_.noc) {
       VTR_ASSERT(noc_cost_handler_.has_value());
       noc_cost_handler_->print_noc_costs("Initial NoC Placement Costs", costs_, noc_opts_);
   }

   if (placer_opts_.place_algorithm.is_timing_driven()) {
       VTR_LOG("Initial placement estimated Critical Path Delay (CPD): %g ns\n",
               1e9 * critical_path_.delay());
       VTR_LOG("Initial placement estimated setup Total Negative Slack (sTNS): %g ns\n",
               1e9 * timing_info_->setup_total_negative_slack());
       VTR_LOG("Initial placement estimated setup Worst Negative Slack (sWNS): %g ns\n",
               1e9 * timing_info_->setup_worst_negative_slack());
       VTR_LOG("\n");
       VTR_LOG("Initial placement estimated setup slack histogram:\n");
       print_histogram(create_setup_slack_histogram(*timing_info_->setup_analyzer()));
   }

   const BlkLocRegistry& blk_loc_registry = placer_state_.blk_loc_registry();
   size_t num_macro_members = 0;
   for (const t_pl_macro& macro : blk_loc_registry.place_macros().macros()) {
       num_macro_members += macro.members.size();
   }
   VTR_LOG("Placement contains %zu placement macros involving %zu blocks (average macro size %f)\n",
           blk_loc_registry.place_macros().macros().size(), num_macro_members,
           float(num_macro_members) / blk_loc_registry.place_macros().macros().size());
   VTR_LOG("\n");
}