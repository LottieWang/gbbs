// This code is part of the project "Theoretically Efficient Parallel Graph
// Algorithms Can Be Fast and Scalable", presented at Symposium on Parallelism
// in Algorithms and Architectures, 2018.
// Copyright (c) 2018 Laxman Dhulipala, Guy Blelloch, and Julian Shun
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all  copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <limits>
#include "pbbslib/random_shuffle.h"
#include "gbbs/pbbslib/resizable_table.h"
#include "gbbs/pbbslib/sparse_table.h"
#include "gbbs/gbbs.h"
#include "gbbs/semiasym/graph_filter.h"

namespace gbbs {
namespace filter_based_scc {

using label_type = uintE;  // at most as many labels as vertices
static constexpr label_type kUnfinished = std::numeric_limits<label_type>::max();

using K = uintE;
using V = uintE;
using T = std::tuple<K, V>;

namespace {

// hash32 is sufficient?
struct hash_kv {
  uint64_t operator()(const K& k) { return pbbslib::hash64(k); }
};

template <class V>
struct First_Search {
  V& visited;
  First_Search(V& _visited) : visited(_visited) {}
  inline bool update(uintE s, uintE d) {
    visited[d] = true;
    return true;
  }
  inline bool updateAtomic(uintE s, uintE d) {
    return pbbslib::CAS(&visited[d], false, true);
  }
  inline bool cond(uintE d) { return !visited[d]; }
};

template <class V>
inline First_Search<V> make_first_search(V& visited) {
  return First_Search<V>(visited);
}

template <class Graph, class Seq>
inline bool* first_search(Graph& GA, Seq& zero, uintE start, const flags fl = 0) {
  using W = typename Graph::weight_type;
  size_t n = GA.n;

  auto Flags = sequence<bool>(n, false);
  Flags[start] = true;

  parallel_for(0, zero.size(), [&] (size_t i) {
    uintE v = zero[i];
    Flags[v] = true;
  });

  auto frontier = vertexSubset(n, start);
  size_t rd = 0;
  while (!frontier.isEmpty()) {
    vertexSubset output = edgeMap(
        GA, frontier, wrap_em_f<W>(make_first_search(Flags)), -1, fl);
    frontier.del();
    frontier = output;
    rd++;
  }
  return Flags.to_array();
}

template <class W, class Seq, class Tab>
struct Search_F {
  Tab& tab;
  Seq& labels;
  bool* bits;
  Search_F(Tab& _tab, Seq& _labels, bool* _bits)
      : tab(_tab), labels(_labels), bits(_bits) {}

  inline bool update(const uintE& s, const uintE& d, const W& wgh) {
    return updateAtomic(s, d, wgh);
  }

  inline bool updateAtomic(const uintE& s, const uintE& d, const W& wgh) {
    // s and d are (currently) in the same subproblem, since we are iteratively
    // pruning the graph's edges.
    // if (labels[s] == labels[d]) {

    bool labels_changed = false;
    auto s_iter = tab.get_iter(s);
    // iterate through s's labels.
    if (s_iter.init()) {
      {
        auto table_entry = s_iter.next();
        uintE s_label = std::get<1>(table_entry);
        labels_changed |= tab.insert(std::make_tuple(d, s_label));
      }

      while (s_iter.has_next()) {
        auto table_entry = s_iter.next();
        uintE s_label = std::get<1>(table_entry);
        labels_changed |= tab.insert(std::make_tuple(d, s_label));
      }
    }
    if (labels_changed) {
      // d should be included in next frontier;
      // CAS to make sure only one ngh from this frontier adds it.
      return pbbslib::CAS(&bits[d], false, true);
    }
    // }
    return false;
  }

  inline bool cond(uintE d) {
    return true;
  }
};

template <class W, class Seq, class Tab>
inline Search_F<W, Seq, Tab> make_search_f(Tab& tab, Seq& labels, bool* bits) {
  return Search_F<W, Seq, Tab>(tab, labels, bits);
}

template <class Graph, class Seq, class VS>
inline pbbslib::resizable_table<K, V, hash_kv> multi_search(Graph& GA,
                                                   Seq& labels, bool* bits,
                                                   VS& frontier,
                                                   size_t label_start,
                                                   const flags fl = 0) {
  using W = typename Graph::weight_type;

  // table stores (vertex, label) pairs
  T empty = std::make_tuple(UINT_E_MAX, UINT_E_MAX);
  size_t backing_size = 1 << pbbslib::log2_up(frontier.size() * 2);
  auto table_backing = sequence<T>(backing_size);
  auto table = pbbslib::resizable_table<K, V, hash_kv>(backing_size, empty, hash_kv(),
                                                       table_backing.to_array(), true);
  frontier.toSparse();
  parallel_for(0, frontier.size(), [&] (size_t i) {
    uintE v = frontier.s[i];
    // each center initially just stores itself.
    table.insert(std::make_tuple(v, label_start + i));
  });
  table.update_nelms();

  size_t rd = 0;
  while (!frontier.isEmpty()) {
    frontier.toSparse();

    auto work_upperbound_seq = pbbs::delayed_seq<size_t>(frontier.size(), [&](size_t i) {
      uintE v = frontier.s[i];
      size_t n_labels = table.num_appearances(v);
      size_t effective_degree = (fl & in_edges) ? GA.get_vertex(v).in_degree()
                                                : GA.get_vertex(v).out_degree();
      return effective_degree * n_labels;
    });

    size_t work_upperbound = pbbslib::reduce_add(work_upperbound_seq);
    table.maybe_resize(work_upperbound);

    parallel_for(0, frontier.size(), [&] (size_t i) {
      uintE v = frontier.s[i];
      bits[v] = 0;  // reset visted flag
    });

    vertexSubset output = edgeMap(
        GA, frontier, make_search_f<W>(table, labels, bits), -1, fl | no_dense);
    table.update_nelms();
    frontier.del();
    frontier = output;
    rd++;
  }
  return table;
}

}  // namespace


template <class Graph>
inline sequence<label_type> StronglyConnectedComponents(Graph& GA, double beta = 1.5) {

  using W = typename Graph::weight_type;
  timer initt;
  initt.start();
  size_t n = GA.n;

  // Everyone's initial label is "unfinished".
  auto labels = sequence<label_type>(n, [](size_t) { return kUnfinished; });

  // TODO: necessary?
  auto ba = sequence<bool>(n, false);
  auto bits = ba.to_array();

  // Split vertices into those with zero in/out degree (zero), and those with non-zero
  // in- and out-degree (non_zero).
  auto v_im = pbbslib::make_sequence<uintE>(n, [](size_t i) { return i; });
  auto zero = pbbslib::filter(v_im, [&](size_t i) {
    return (GA.get_vertex(i).out_degree() == 0) || (GA.get_vertex(i).in_degree() == 0);
  });
  auto non_zero = pbbslib::filter(v_im, [&](size_t i) {
    return (GA.get_vertex(i).out_degree() > 0) && (GA.get_vertex(i).in_degree() > 0);
  });

  // Vertices in non_zero are the candidate centers that the algorithm will
  // iteratively add, and run searches from. First permute them.
  auto P = pbbslib::random_shuffle(non_zero);
  std::cout << "Filtered: " << zero.size()
            << " vertices. Num remaining = " << P.size() << "\n";

  // Assign unique labels to each vertex in zero (in their own SCCs).
  // Labels are in [0...zero.size()).
  parallel_for(0, zero.size(), [&] (size_t i) { labels[zero[i]] = i; });

  size_t step_size = 1, cur_offset = 0, finished = 0, cur_round = 0;
  double step_multiplier = beta;
  size_t label_offset = zero.size();


  // The packed graph that the algorithm iteratively filters.
  timer pg_init; pg_init.start();
  auto PG = gbbs::sage::build_asymmetric_packed_graph(GA);
  pg_init.stop(); pg_init.reportTotal("packed graph creation time");

  initt.stop();
  initt.reportTotal("init");

  // Run the first search (using two BFSs)
  {
    timer hd; hd.start();
    // Find the vertex with largest (in- + out-) degree (a guess---the hope is
    // that it's in the largest component).
    auto deg_im_f = [&](size_t i) {
      return std::make_tuple(i, GA.get_vertex(i).out_degree() + GA.get_vertex(i).in_degree());
    };
    auto deg_im = pbbslib::make_sequence<std::tuple<uintE, uintE>>(n, deg_im_f);
    auto red_f = [](const std::tuple<uintE, uintE>& l,
                    const std::tuple<uintE, uintE>& r) {
          return (std::get<1>(l) > std::get<1>(r)) ? l : r;
    };
    auto id = std::make_tuple<uintE, uintE>(0, 0);
    auto monoid = pbbslib::make_monoid(red_f, id);
    std::tuple<uintE, uintE> vertex_and_degree =
        pbbslib::reduce(deg_im, monoid);
    uintE start = std::get<0>(vertex_and_degree);

    // Run the first search from start to identify the SCC containing it.
    if (labels[start] == kUnfinished) {
      auto visited_in  = first_search(GA, zero, start, in_edges);
      auto visited_out = first_search(GA, zero, start);

      size_t start_label = label_offset;  // The label of the SCC containing start.
      par_for(0, n, [&] (size_t i) {
        bool inv = visited_in[i];
        bool outv = visited_out[i];
        // Do not overwrite labels of isolated-SCC vertices (in zero). These are
        // the only vertices whose label can be kUnfinished before this loop.
        if (inv && outv && labels[i] == kUnfinished) {
          labels[i] = start_label;  // In start's SCC.
        }
      });

      // Prune remaining edges based on intersection with start (O(m) work).
      auto pred_f = [&] (const uintE& u, const uintE& v, const W& wgh) {
        // (1) Remove edges between vertices already in different SCCs.
        bool cond_1 = labels[u] != labels[v];
        // (2,3) Remove edges crossing different subproblems.
        bool cond_2 = visited_in[u] != visited_in[v];
        bool cond_3 = visited_out[u] != visited_out[v];
        return !(cond_1 || cond_2 || cond_3);
      };

      std::cout << "PG.m was: " << PG.m << std::endl;
      timer fg; fg.start();
      PG.clear_vertices([&] (size_t i) { return labels[i] == kUnfinished; });
      gbbs::sage::filter_graph(PG, pred_f);
      fg.stop(); fg.reportTotal("Filter Graph (first) time");
      std::cout << "PG.m is now: " << PG.m << std::endl;

      pbbslib::free_array(visited_in);
      pbbslib::free_array(visited_out);
      label_offset += 1;
      hd.stop();
      hd.reportTotal("big scc time");
    }
  }

  timer MS; MS.start();

  auto Q = pbbslib::filter(P, [&](uintE v) { return labels[v] == kUnfinished; });
  std::cout << "After first round, Q = " << Q.size()
            << " vertices remain. Total done = " << (n - Q.size()) << "\n";

  timer CT;
  timer clear_vertices_t;

  auto in_sizes = sequence<uintE>(n, UINT_E_MAX);
  auto out_sizes = sequence<uintE>(n, UINT_E_MAX);

  while (finished < Q.size()) {
    timer rt;
    rt.start();

    // Run searches between P[cur_offset, end).
    size_t end = std::min(cur_offset + step_size, Q.size());
    size_t vs_size = end - cur_offset;
    finished += vs_size;
    step_size = ceil(step_size * step_multiplier);
    cur_round++;
    size_t round_offset = cur_offset;
    cur_offset += vs_size;

    auto centers_pre_filter = pbbslib::make_sequence<uintE>(
        vs_size, [&](size_t i) { return Q[round_offset + i]; });
    auto centers = pbbslib::filter(
        centers_pre_filter, [&](uintE v) { return labels[v] == kUnfinished; });

    std::cout << "round = " << cur_round << " n_centers = " << centers.size()
              << " originally was " << vs_size
              << " centers. Total vertices remaining = "
              << (Q.size() - finished) << "\n";

    if (centers.size() == 0) continue;

    size_t cur_label_offset = label_offset;
    label_offset += centers.size();

    if (cur_round == 1) {
      timer ft;
      ft.start();
      uintE start = centers[0];

      auto visited_in  = first_search(PG, zero, start, in_edges);
      auto visited_out = first_search(PG, zero, start);

      size_t start_label = label_offset;  // The label of the SCC containing start.
      par_for(0, n, [&] (size_t i) {
        bool inv = visited_in[i];
        bool outv = visited_out[i];
        // Do not overwrite labels of isolated-SCC vertices (in zero). These are
        // the only vertices whose label can be kUnfinished before this loop.
        if (inv && outv && labels[i] == kUnfinished) {
          labels[i] = start_label;  // In start's SCC.
        }
      });

      // Prune remaining edges based on intersection with start (O(m) work).
      auto pred_f = [&] (const uintE& u, const uintE& v, const W& wgh) {
        // (1) Remove edges between vertices already in different SCCs.
        bool cond_1 = labels[u] != labels[v];
        // (2,3) Remove edges crossing different subproblems.
        bool cond_2 = visited_in[u] != visited_in[v];
        bool cond_3 = visited_out[u] != visited_out[v];
        return !(cond_1 || cond_2 || cond_3);
      };

      std::cout << "PG.m was: " << PG.m << std::endl;
      auto labeled = [&] (size_t i) { return labels[i] == kUnfinished; };
      CT.start();
      clear_vertices_t.start();
      PG.clear_vertices(labeled);
      clear_vertices_t.stop();
      gbbs::sage::filter_graph(PG, pred_f);
      CT.stop();
      std::cout << "PG.m is now: " << PG.m << std::endl;

      pbbslib::free_array(visited_in);
      pbbslib::free_array(visited_out);
      label_offset += 1;
      ft.stop();
      ft.reportTotal("first scc time");
    }

    timer ins; ins.start();
    auto centers_copy = centers;
    size_t centers_size = centers.size();
    auto in_f = vertexSubset(n, centers_size, centers.to_array());
    auto in_table =
        multi_search(PG, labels, bits, in_f, cur_label_offset, in_edges);
    std::cout << "Finished in search"
              << "\n";
    ins.stop(); ins.reportTotal("insearch time");

    timer outs; outs.start();
    auto out_f = vertexSubset(n, centers_size, centers_copy.to_array());
    auto out_table = multi_search(PG, labels, bits, out_f, cur_label_offset);
    std::cout << "in_table, m = " << in_table.m << " ne = " << in_table.ne
              << "\n";
    std::cout << "out_table, m = " << out_table.m << " ne = " << out_table.ne
              << "\n";
    outs.stop(); outs.reportTotal("outsearch time");

    auto& smaller_t = (in_table.m <= out_table.m) ? in_table : out_table;
    auto& larger_t = (in_table.m > out_table.m) ? in_table : out_table;

    // intersect the tables
    auto map_f = [&](const std::tuple<K, V>& kev) {
      uintE v = std::get<0>(kev);
      label_type label = std::get<1>(kev);
      if (larger_t.contains(v, label)) {
        // in 'label' scc
        // Min visitor from this StronglyConnectedComponents acquires it.
        pbbslib::write_min(&labels[v], label);
      }
      // else {  // No longer need to set subproblems
      //   pbbslib::write_max(&labels[v], label);
      // }
    };
    smaller_t.map(map_f);

    auto in_init_num_appear_f = [&] (const std::tuple<K, V>& kev) {
      uintE v = std::get<0>(kev);
      if (in_sizes[v] == UINT_E_MAX) in_sizes[v] = in_table.num_appearances(v);
    };
    auto out_init_num_appear_f = [&] (const std::tuple<K, V>& kev) {
      uintE v = std::get<0>(kev);
      if (out_sizes[v] == UINT_E_MAX) out_sizes[v] = out_table.num_appearances(v);
    };
    auto in_reset = [&] (const std::tuple<K, V>& kev) {
      uintE v = std::get<0>(kev);
      if (in_sizes[v] != UINT_E_MAX) in_sizes[v] = UINT_E_MAX;
    };
    auto out_reset = [&] (const std::tuple<K, V>& kev) {
      uintE v = std::get<0>(kev);
      if (out_sizes[v] != UINT_E_MAX) out_sizes[v] = UINT_E_MAX;
    };

    in_table.map(in_init_num_appear_f);
    out_table.map(out_init_num_appear_f);

    clear_vertices_t.start();
    //PG.clear_vertices([&] (size_t v) { return labels[v] == kUnfinished; });
    PG.clear_vertices(centers, [&](size_t v) { return labels[v] == kUnfinished; });
    clear_vertices_t.stop();



    size_t remaining = Q.size() - finished + vs_size;
    auto to_process = pbbslib::make_sparse_table(remaining, std::make_tuple(UINT_E_MAX, pbbs::empty()), [&] (const K& k) { return pbbs::hash64(k); });
    auto in_insert_t = [&] (const std::tuple<K, V>& kev) {
      auto k = std::get<0>(kev);
      auto insert_fringe = [&] (const uintE& u, const uintE& v, const W& wgh) {
        if (!in_table.contains(v)) {
          to_process.insert(std::make_tuple(v, pbbs::empty()));
        }
      };
      if (to_process.insert(std::make_tuple(k, pbbs::empty()))) {
        // insert only out neighbors
        PG.get_vertex(k).out_neighbors().map(insert_fringe);
      }
    };
    in_table.map(in_insert_t);

    auto out_insert_t = [&] (const std::tuple<K, V>& kev) {
      auto k = std::get<0>(kev);
      auto insert_fringe = [&] (const uintE& u, const uintE& v, const W& wgh) {
        if (!out_table.contains(v)) {
          to_process.insert(std::make_tuple(v, pbbs::empty()));
        }
      };
      if (to_process.insert(std::make_tuple(k, pbbs::empty()))) {
        // insert only out neighbors
        PG.get_vertex(k).in_neighbors().map(insert_fringe);
      }
    };
    out_table.map(out_insert_t);

    auto elts = to_process.entries();
    std::cout << "Num elements to process is: " << elts.size() << std::endl;

    auto pred_f = [&] (const uintE& u, const uintE& v, const W& wgh) {
      if (labels[u] != labels[v]) return false;

      // otherwise, they still have the same label
      assert(labels[u] == kUnfinished); assert(labels[v] == kUnfinished);

      // generalization of cond_2 and cond_3 from above to multiple searches.
      if (in_sizes[u] != in_sizes[v]) return false;
      return out_sizes[u] == out_sizes[v];
    };

    // Prune the graph.
    CT.start();
    auto elts_seq = pbbs::delayed_seq<uintE>(elts.size(), [&] (size_t i) {
      return std::get<0>(elts[i]);
    });
    PG.filter_graph(pred_f, elts_seq);
    // gbbs::sage::filter_graph(PG, pred_f);
    CT.stop();

    in_table.map(in_reset);
    out_table.map(out_reset);

//    // set the subproblems
//    auto sp_map = [&](const std::tuple<K, V>& kev) {
//      uintE v = std::get<0>(kev);
//      size_t label = std::get<1>(kev);
//      // note that if v is already in an StronglyConnectedComponents (from (1)), the pbbslib::write_max will
//      // read, compare and fail, as the top bit is already set.
//      pbbslib::write_max(&labels[v], label);
//    };
//    larger_t.map(sp_map);

    in_table.del();
    out_table.del();

    rt.stop();
    rt.reportTotal("Round time");
  }

  MS.stop();
  MS.reportTotal("MultiSearch Time");
  clear_vertices_t.reportTotal("Clear Vertices time");
  CT.reportTotal("Compression time");

  return labels;
}

}  // namespace filter_based_scc
}  // namespace gbbs
