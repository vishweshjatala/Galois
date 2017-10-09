/** KCore -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2017, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Compute KCore on distributed Galois using top-filter.
 *
 * @author Loc Hoang <l_hoang@utexas.edu>
 */

/******************************************************************************/
/* Sync code/calls was manually written, not compiler generated */
/******************************************************************************/

#include <iostream>
#include <limits>
#include "galois/DistGalois.h"
#include "galois/gstl.h"
#include "DistBenchStart.h"
#include "galois/DistAccumulator.h"
#include "galois/runtime/Tracer.h"

#ifdef __GALOIS_HET_CUDA__
#include "gen_cuda.h"
struct CUDA_Context *cuda_ctx;
#endif

constexpr static const char* const REGION_NAME = "KCore";

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/
namespace cll = llvm::cl;
static cll::opt<unsigned int> maxIterations("maxIterations", 
                               cll::desc("Maximum iterations: Default 10000"), 
                               cll::init(10000));
// required k specification for k-core
static cll::opt<unsigned int> k_core_num("kcore",
                                     cll::desc("KCore value"),
                                     cll::Required);

/******************************************************************************/
/* Graph structure declarations + other inits */
/******************************************************************************/

struct NodeData {
  std::atomic<uint32_t> current_degree;
  std::atomic<uint32_t> trim;
  uint8_t flag;
};

typedef hGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;

// bitset for tracking updates
galois::DynamicBitSet bitset_current_degree;
galois::DynamicBitSet bitset_trim;

// add all sync/bitset structs (needs above declarations)
#include "gen_sync.hh"

/******************************************************************************/
/* Functors for running the algorithm */
/******************************************************************************/

/* Degree counting
 * Called by InitializeGraph1 */
struct InitializeGraph2 {
  Graph *graph;

  InitializeGraph2(Graph* _graph) : graph(_graph){}

  /* Initialize the entire graph node-by-node */
  void static go(Graph& _graph) {
    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

  #ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      std::string impl_str("CUDA_DO_ALL_IMPL_InitializeGraph2_" + 
                           (_graph.get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      InitializeGraph2_cuda(*nodesWithEdges.begin(), *nodesWithEdges.end(),
                            cuda_ctx);
      StatTimer_cuda.stop();
    } else if (personality == CPU)
  #endif
    galois::do_all(
      galois::iterate(nodesWithEdges),
      InitializeGraph2{ &_graph },
      galois::loopname(_graph.get_run_identifier("InitializeGraph2").c_str()),
      galois::timeit(),
      galois::no_stats()
    );

    _graph.sync<writeDestination, readSource, Reduce_add_current_degree, 
      Broadcast_current_degree, Bitset_current_degree>("InitializeGraph2");
  }

  /* Calculate degree of nodes by checking how many nodes have it as a dest and
   * adding for every dest */
  void operator()(GNode src) const {
    for (auto current_edge : graph->edges(src)) {
      GNode dest_node = graph->getEdgeDst(current_edge);

      NodeData& dest_data = graph->getData(dest_node);
      galois::atomicAdd(dest_data.current_degree, (uint32_t)1);

      bitset_current_degree.set(dest_node);
    }
  }
};


/* Initialize: initial field setup */
struct InitializeGraph1 {
  Graph *graph;

  InitializeGraph1(Graph* _graph) : graph(_graph){}

  /* Initialize the entire graph node-by-node */
  void static go(Graph& _graph) {
    const auto& allNodes = _graph.allNodesRange();

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      std::string impl_str("CUDA_DO_ALL_IMPL_InitializeGraph1_" + 
                           (_graph.get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      InitializeGraph1_cuda(*(allNodes.begin()), *(allNodes.end()), 
                            cuda_ctx);
      StatTimer_cuda.stop();
    } else if (personality == CPU)
  #endif
     galois::do_all(
        galois::iterate(allNodes.begin(), allNodes.end()),
        InitializeGraph1{ &_graph },
        galois::loopname(_graph.get_run_identifier("InitializeGraph1").c_str()),
        galois::timeit(),
        galois::no_stats()
      );

    // degree calculation
    InitializeGraph2::go(_graph);
  }

  /* Setup intial fields */
  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);
    src_data.flag = true;
    src_data.trim = 0;
    src_data.current_degree = 0;
  }
};


/* Use the trim value (i.e. number of incident nodes that have been removed)
 * to update degrees.
 * Called by KCoreStep1 */
struct KCoreStep2 {
  Graph* graph;

  KCoreStep2(Graph* _graph) : graph(_graph){}

  void static go(Graph& _graph){
    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();
  #ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      std::string impl_str("CUDA_DO_ALL_IMPL_KCore_" + 
                           (_graph.get_run_identifier()));
      galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
      StatTimer_cuda.start();
      KCoreStep2_cuda(*nodesWithEdges.begin(), *nodesWithEdges.end(), cuda_ctx);
      StatTimer_cuda.stop();
    } else if (personality == CPU)
  #endif
     galois::do_all(
       galois::iterate(nodesWithEdges.begin(), nodesWithEdges.end()),
       KCoreStep2{ &_graph },
       galois::loopname(_graph.get_run_identifier("KCore").c_str()),
       galois::timeit(),
       galois::no_stats()
     );
  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    // we currently do not care about degree for dead nodes, 
    // so we ignore those (i.e. if flag isn't set, do nothing)
    if (src_data.flag) {
      if (src_data.trim > 0) {
        src_data.current_degree = src_data.current_degree - src_data.trim;
      }
    }

    src_data.trim = 0;
  }
};


/* Step that determines if a node is dead and updates its neighbors' trim
 * if it is */
struct KCoreStep1 {
  cll::opt<uint32_t>& local_k_core_num;
  Graph* graph;

  galois::DGAccumulator<unsigned int>& DGAccumulator_accum;

  KCoreStep1(cll::opt<uint32_t>& _kcore, Graph* _graph,
             galois::DGAccumulator<unsigned int>& _dga) : 
    local_k_core_num(_kcore), graph(_graph), DGAccumulator_accum(_dga) {}

  void static go(Graph& _graph, galois::DGAccumulator<unsigned int>& dga) {
    unsigned iterations = 0;
    
    const auto& nodesWithEdges = _graph.allNodesWithEdgesRange();

    do {
      _graph.set_num_iter(iterations);
      dga.reset();
    #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        std::string impl_str("CUDA_DO_ALL_IMPL_KCore_" + 
                             (_graph.get_run_identifier()));
        galois::StatTimer StatTimer_cuda(impl_str.c_str(), REGION_NAME);
        StatTimer_cuda.start();
        int __retval = 0;
        KCoreStep1_cuda(*nodesWithEdges.begin(), *nodesWithEdges.end(),
                        __retval, k_core_num, cuda_ctx);
        dga += __retval;
        StatTimer_cuda.stop();
      } else if (personality == CPU)
    #endif
      galois::do_all(
        galois::iterate(nodesWithEdges),
        KCoreStep1{ k_core_num, &_graph, dga },
        galois::loopname(_graph.get_run_identifier("KCore").c_str()),
        galois::timeit(),
        galois::no_stats()
      );

      // do the trim sync; readSource because in symmetric graph 
      // source=destination; not a readAny because any will grab non 
      // source/dest nodes (which have degree 0, so they won't have a trim 
      // anyways)
      _graph.sync<writeDestination, readSource, Reduce_add_trim, Broadcast_trim, 
                  Bitset_trim>("KCore");

      // handle trimming (locally)
      KCoreStep2::go(_graph);

      iterations++;
    } while ((iterations < maxIterations) && dga.reduce(_graph.get_run_identifier()));

    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      galois::runtime::reportStat_Single(REGION_NAME, 
        "NUM_ITERATIONS_" + std::to_string(_graph.get_run_num()), 
        (unsigned long)iterations);
    }

  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    // only if node is alive we do things
    if (src_data.flag) {
      if (src_data.current_degree < local_k_core_num) {
        // set flag to 0 (false) and increment trim on outgoing neighbors
        // (if they exist)
        src_data.flag = false;
        DGAccumulator_accum += 1; // can be optimized: node may not have edges

        for (auto current_edge : graph->edges(src)) {
           GNode dst = graph->getEdgeDst(current_edge);

           auto& dst_data = graph->getData(dst);

           galois::atomicAdd(dst_data.trim, (uint32_t)1);
           bitset_trim.set(dst);
        }
      }
    }
  }
};

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Gets the total number of nodes that are still alive */
struct GetAliveDead {
  Graph* graph;
  galois::DGAccumulator<uint64_t>& DGAccumulator_accum;
  galois::DGAccumulator<uint64_t>& DGAccumulator_accum2;

  GetAliveDead(Graph* _graph, 
               galois::DGAccumulator<uint64_t>& _DGAccumulator_accum,
               galois::DGAccumulator<uint64_t>& _DGAccumulator_accum2) : 
      graph(_graph), DGAccumulator_accum(_DGAccumulator_accum),
      DGAccumulator_accum2(_DGAccumulator_accum2) {}

  void static go(Graph& _graph,
    galois::DGAccumulator<uint64_t>& dga1,
    galois::DGAccumulator<uint64_t>& dga2) {
  #ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      // TODO currently no GPU support for sanity check operator
      fprintf(stderr, "Warning: No GPU support for sanity check; might get "
                      "wrong results.\n");
    }
  #endif
    dga1.reset();
    dga2.reset();

    galois::do_all(galois::iterate(_graph.allNodesRange().begin(), _graph.allNodesRange().end()), 
                   GetAliveDead(&_graph, dga1, dga2), 
                   galois::loopname("GetAliveDead"),
                   galois::no_stats());

    uint32_t num_alive = dga1.reduce();
    uint32_t num_dead = dga2.reduce();

    // Only node 0 will print data
    if (galois::runtime::getSystemNetworkInterface().ID == 0) {
      printf("Number of nodes alive is %u, dead is %u\n", num_alive,
             num_dead);
    }
  }

  /* Check if an owned node is alive/dead: increment appropriate accumulator */
  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    if (graph->isOwned(graph->getGID(src))) {
      if (src_data.flag) {
        DGAccumulator_accum += 1;
      } else {
        DGAccumulator_accum2 += 1;
      }
    }
  }
};

/******************************************************************************/
/* Main method for running */
/******************************************************************************/

constexpr static const char* const name = "KCore - Distributed Heterogeneous "
                                          "Push Filter.";
constexpr static const char* const desc = "KCore on Distributed Galois.";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  auto& net = galois::runtime::getSystemNetworkInterface();
  if (net.ID == 0) {
    galois::runtime::reportParam(REGION_NAME, "Max Iterations", 
                                (unsigned long)maxIterations);
  }

  galois::StatTimer StatTimer_total("TIMER_TOTAL", REGION_NAME);

  StatTimer_total.start();

  #ifdef __GALOIS_HET_CUDA__
  Graph* h_graph = symmetricDistGraphInitialization<NodeData, void>(&cuda_ctx);
  #else
  Graph* h_graph = symmetricDistGraphInitialization<NodeData, void>();
  #endif

  bitset_current_degree.resize(h_graph->size());
  bitset_trim.resize(h_graph->size());

  galois::gPrint("[", net.ID, "] InitializeGraph::go functions called\n");
  galois::StatTimer StatTimer_graph_init("TIMER_GRAPH_INIT", REGION_NAME);
  StatTimer_graph_init.start();
    InitializeGraph1::go((*h_graph));
  StatTimer_graph_init.stop();
  galois::runtime::getHostBarrier().wait();

  galois::DGAccumulator<unsigned int> DGAccumulator_accum;
  galois::DGAccumulator<uint64_t> dga1;
  galois::DGAccumulator<uint64_t> dga2;

  for (auto run = 0; run < numRuns; ++run) {
    galois::gPrint("[", net.ID, "] KCoreStep1::go run ", run, " called\n");
    std::string timer_str("TIMER_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), REGION_NAME);

    StatTimer_main.start();
      KCoreStep1::go(*h_graph, DGAccumulator_accum);
    StatTimer_main.stop();

    // sanity check
    GetAliveDead::go(*h_graph, dga1, dga2);

    // re-init graph for next run
    if ((run + 1) != numRuns) {
      (*h_graph).set_num_run(run+1);

      #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) { 
        bitset_current_degree_reset_cuda(cuda_ctx);
        bitset_trim_reset_cuda(cuda_ctx);
      } else
      #endif
      { bitset_current_degree.reset();
      bitset_trim.reset(); }

      InitializeGraph1::go((*h_graph));
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();

  // Verify, i.e. print out graph data for examination
  if (verify) {
    #ifdef __GALOIS_HET_CUDA__
    if (personality == CPU) { 
    #endif
      for (auto ii = (*h_graph).masterNodesRange().begin(); 
                ii != (*h_graph).masterNodesRange().end(); 
                ++ii) {
        // prints the flag (alive/dead)
        galois::runtime::printOutput("% %\n", (*h_graph).getGID(*ii), 
                                     (bool)(*h_graph).getData(*ii).flag);

        // does a sanity check as well: 
        // degree higher than kcore if node is alive
        if (!((*h_graph).getData(*ii).flag)) {
          assert((*h_graph).getData(*ii).current_degree < k_core_num);
        } 
      }
    #ifdef __GALOIS_HET_CUDA__
    } else if (personality == GPU_CUDA) {
      for (auto ii = (*h_graph).masterNodesRange().begin(); 
                ii != (*h_graph).masterNodesRange().end(); 
                ++ii) {
        galois::runtime::printOutput("% %\n", (*h_graph).getGID(*ii), 
                                   (bool)get_node_flag_cuda(cuda_ctx, *ii));
      }
    }
    #endif
  }

  return 0;
}