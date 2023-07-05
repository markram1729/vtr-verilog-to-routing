#ifndef VTR_UNIFORM_INTER_LAYER_MOVE_GENERATOR_H
#define VTR_UNIFORM_INTER_LAYER_MOVE_GENERATOR_H

#include "move_generator.h"
#include "timing_place.h"

/**
 * @brief Uniform inter-layer move generator
 *
 * randomly picks a from_block with equal probabilities for all blocks, and then moves it randomly within
 * a range limit centered on from_block in the compressed block grid space
 */

class UniformInterLayerMoveGenerator : public MoveGenerator {
    e_create_move propose_move(t_pl_blocks_to_be_moved& blocks_affected, e_move_type& /*move_type*/, t_logical_block_type& blk_type, float rlim, const t_placer_opts& /*placer_opts*/, const PlacerCriticalities* /*criticalities*/);
};

#endif //VTR_UNIFORM_INTER_LAYER_MOVE_GENERATOR_H