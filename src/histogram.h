// This code is part of the Problem Based Benchmark Suite (PBBS)
// Copyright (c) 2010-2016 Guy Blelloch, Laxman Dhulipala, and the PBBS team
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights (to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once
#include <math.h>
#include <stdio.h>
#include <cstdint>

#include "../lib/counting_sort_no_transpose.h"
#include "sequential_ht.h"

namespace pbbs {

  // Tunable parameters
  constexpr const size_t _hist_max_buckets = 1024;
  constexpr const size_t _hist_seq_threshold = 2048;

  template <typename E, class B>
  struct get_bucket {
    tuple<E,int>* hash_table;
    size_t table_mask;
    size_t low_mask;
    size_t bucket_mask;
    int num_buckets;
    int k;
    B& I;

    tuple<E*,int> heavy_hitters(size_t n, size_t count) {
      E* sample = newA(E, count);
      for (size_t i = 0; i < count; i++) {
        sample[i] = I[hash64(i)%n];
      }
      std::sort(sample,sample+count);

      // only keep those with at least three copies
      int k = 0;
      int c = 0;
      for (size_t i = 1; i < count; i++) {
        if (sample[i] == sample[i-1]) {
          if (c++ == 1) {
            sample[k++] = sample[i];
          }
        } else {
          c = 0;
        }
      }
      return make_tuple(sample,k);
    }

    tuple<E,int>* make_hash_table(E* entries, size_t n,
         size_t table_size, size_t table_mask) {
      using ttype = tuple<E, int>;
      auto table = newA(ttype, table_size);
      for (size_t i=0; i < table_size; i++)
        table[i] = make_pair(0,-1);
      size_t n_distinct = 0;
      for (size_t i = 0; i < n; i++) {
        size_t h = hash64(entries[i])&table_mask;
        while (get<1>(table[h]) != -1) {
          h = (h+1)&table_mask;
        }
        table[h] = make_pair(entries[i],n_distinct++);
      }
      return table;
    }

    get_bucket(B& A, size_t n, size_t bits) : I(A) {
      num_buckets = 1 << bits;
      bucket_mask = num_buckets-1;
      low_mask = ~((size_t) 15);
      int count = 2 * num_buckets;
      int table_size = 4 * count; // tune
      table_mask = table_size-1;

      E* sample;
      std::tie(sample, k) = heavy_hitters(n, count);

      if (k > 0) {
        hash_table = make_hash_table(sample, k, table_size, table_mask);
      }
      free(sample);
    }

    ~get_bucket() {
      if (k > 0) { free(hash_table);} }

    size_t operator() (size_t i) {
      if (k > 0) {
        size_t h = pbbs::hash64(I[i])&table_mask;
        while (true) {
          E elm; int ct;
          std::tie(elm, ct) = hash_table[h];
          if (ct == -1) { break; }
          if (elm == I[i] && ct != -1) {
            return ct + num_buckets;
          }
          h = (h+1)&table_mask;
        }
      }
      return pbbs::hash64(I[i] & low_mask) & bucket_mask;
    }
  };

  template <class K, class V>
  struct hist_table {
    using KV = tuple<K, V>;
    KV empty;
    KV* table;
    size_t size;
    hist_table(KV _empty, size_t _size) : empty(_empty), size(_size) {
      table = newA(KV, size);
      parallel_for (size_t i=0; i<size; i++) { table[i] = empty; }
    }
    hist_table() {}

    void resize(size_t req_size) {
      if (req_size > size) {
        free(table);
        table = newA(KV, req_size);
        size = req_size;
        parallel_for (size_t i=0; i<size; i++) { table[i] = empty; }
        cout << "resized to: " << size << endl;
      }
    }

    void del() {
      if (table) {
        free(table);
      }
    }
  };

  template <class O, class K, class V, class A, class Apply>
  inline pair<size_t, O*> histogram_medium(A& get_key, size_t n, Apply& apply_f, hist_table<K, V>& ht) {
    using KV = tuple<K, V>;
    size_t sqrt = (size_t) ceil(pow(n,0.5));
    size_t num_buckets = (size_t) (n < 20000000) ? (sqrt/5) : sqrt;

    num_buckets = std::max(1 << log2_up(num_buckets), 1);
    num_buckets = min(num_buckets, _hist_max_buckets);
    size_t bits = log2_up(num_buckets);

    // (1) count-sort based on bucket
    size_t low_mask = ~((size_t)15);
    size_t bucket_mask = num_buckets - 1;
    auto gb = [&] (uintE i) {
      return pbbs::hash64(get_key[i] & low_mask) & bucket_mask;
    };

    K* elms; size_t* counts; size_t num_blocks;
    if (num_buckets <= 256) {
      std::tie(elms, counts, num_blocks) = _count_sort<uint8_t, size_t, K>(get_key, gb, n, (uintE)num_buckets);
    } else {
      std::tie(elms, counts, num_blocks) = _count_sort<uint16_t, size_t, K>(get_key, gb, n, (uintE)num_buckets);
    }
    size_t block_size = ((n-1)/num_blocks) + 1;

#define S_STRIDE 64
    size_t* bkt_counts = new_array_no_init<size_t>(num_buckets*S_STRIDE);
    granular_for(i, 0, num_buckets, num_buckets>1, {
      bkt_counts[i*S_STRIDE] = 0;
      if (i == (num_buckets-1)) {
        size_t ct = 0;
        for (size_t j=0; j<num_blocks; j++) {
          size_t start = std::min(j * block_size, n);
          size_t end =  std::min(start + block_size, n);
          ct += (end - start) - counts[j*num_buckets+i];
        }
        bkt_counts[i*S_STRIDE] = ct;
      } else {
        size_t ct = 0;
        for (size_t j=0; j<num_blocks; j++) {
          ct += counts[j*num_buckets+i+1] - counts[j*num_buckets+i];
        }
        bkt_counts[i*S_STRIDE] = ct;
      }
    });

    array_imap<size_t> out_offs = array_imap<size_t>(num_buckets+1);
    array_imap<size_t> ht_offs = array_imap<size_t>(num_buckets+1);
    auto empty = ht.empty;
    KV* table = ht.table;

    // (2) process each bucket, compute the size of each HT and scan (seq)
    ht_offs[0] = 0;
    size_t min_size = std::numeric_limits<size_t>::max();
    size_t max_size = 0;
    size_t avg_size = std::ceil(n/num_buckets);
    for (size_t i=0; i<num_buckets; i++) {
      size_t size = bkt_counts[i*S_STRIDE];
      size_t ht_size = 0;
      if (size > 0) {
        ht_size = 1 << pbbs::log2_up((intT)(size + 1));
      }
      ht_offs[i+1] = ht_offs[i] + ht_size;
      if (size < min_size) {
        min_size = size;
      }
      if (size > max_size) {
        max_size = size;
      }
    }
//    if (n > 100000) {
//      cout << "n = " << n << " min size = " << min_size << " max size = " << max_size << " avg size = " << avg_size << endl;
//    }

    ht.resize(ht_offs[num_buckets]);

    // (3) insert elms into per-bucket hash table (par)
    parallel_for(size_t i=0; i<num_buckets; i++) {
      size_t ht_start = ht_offs[i];
      size_t ht_size = ht_offs[i+1] - ht_start;

      size_t k = 0;
      if (ht_size > 0) {
        KV* my_ht = &(table[ht_start]);
        sequentialHT<K, V> S(my_ht, ht_size, empty);

        for (size_t j=0; j<num_blocks; j++) {
          size_t start = std::min(j * block_size, n);
          size_t end =  std::min(start + block_size, n);
          size_t ct = 0;
          size_t off = 0;
          if (i == (num_buckets-1)) {
            off = counts[j*num_buckets+i];
            ct = (end - start) - off;
          } else {
            off = counts[j*num_buckets+i];
            ct = counts[j*num_buckets+i+1] - off;
          }
          off += start;
          for (size_t k=0; k<ct; k++) {
            K a = elms[off + k];
            S.insertAdd(a);
          }
        }

        k = S.compactIntoSelf(apply_f);
      }
      out_offs[i] = k;
    }

    // (4) scan
    size_t ct = 0;
    for (size_t i=0; i<num_buckets; i++) {
      size_t s = ct;
      ct += out_offs[i];
      out_offs[i] = s;
    }
    out_offs[num_buckets] = ct;
    uintT num_distinct = ct;

    O* res = newA(O, ct);

    // (5) map compacted hts to output, clear hts
    granular_for(i, 0, num_buckets, (num_buckets>1), {
      size_t o = out_offs[i];
      size_t k = out_offs[(i+1)] - o;

      size_t ht_start = ht_offs[i];
      size_t ht_size = ht_offs[i+1] - ht_start;

      if (ht_size > 0) {
        KV* my_ht = &(table[ht_start]);

        for (size_t j=0; j<k; j++) {
          res[o+j] = my_ht[j];
          my_ht[j] = empty;
        }
      }
    });

    free(elms); free(counts); free(bkt_counts);
    return make_pair(num_distinct, res);
  }

  template <class O, class K, class V, class A, class Apply>
  inline pair<size_t, O*> histogram(A& get_key, size_t n, Apply& apply_f, hist_table<K, V>& ht) {
    using KV = tuple<K, V>;
    int nworkers = getWorkers();

    if (n < _hist_seq_threshold  || nworkers == 1) {
      size_t pn = pbbs::log2_up((intT)(n + 1));
      size_t rs = 1L << pn;
      ht.resize(rs);
      sequentialHT<K, V> S(ht.table, n, 1.0f, ht.empty);
      size_t ct = 0;
      for (size_t i=0; i<n; i++) {
        K k = get_key(i);
        ct += S.insertAdd(k);
      }
      O* out = newA(O, ct);
      size_t k = S.compactInto(apply_f, out);
      return make_pair(k, out);
    }

    if (n < 5000000) {
      return histogram_medium<O>(get_key, n, apply_f, ht);
    }

    size_t sqrt = (size_t) ceil(pow(n,0.5));
    size_t num_buckets = (size_t) (n < 20000000) ? (sqrt/5) : sqrt;

    num_buckets = std::max(1 << log2_up(num_buckets), 1);
    num_buckets = min(num_buckets, _hist_max_buckets);
    size_t bits = log2_up(num_buckets);

    auto gb = get_bucket<K, A>(get_key, n, bits);
    size_t num_heavy = gb.k;
    if (num_heavy == 0) {
      return histogram_medium<O>(get_key, n, apply_f, ht);
    }

    bool heavy = (num_heavy > 0);
    size_t num_total_buckets = (heavy) ? 2*num_buckets : num_buckets;
    size_t num_actual_buckets = num_buckets + num_heavy;
//    cout << "gb.k = " << num_heavy << " num bkt = " << num_buckets << endl;

    K* elms; size_t* counts; size_t num_blocks;
    if (num_total_buckets <= 256) {
      std::tie(elms, counts, num_blocks) = _count_sort<uint8_t, size_t, K>(get_key, gb, n, (uintE)num_total_buckets);
    } else {
      std::tie(elms, counts, num_blocks) = _count_sort<uint16_t, size_t, K>(get_key, gb, n, (uintE)num_total_buckets);
    }

    size_t block_size = ((n-1)/num_blocks) + 1;

#define S_STRIDE 64
    size_t* bkt_counts = new_array_no_init<size_t>(num_total_buckets*S_STRIDE);
    granular_for(i, 0, num_actual_buckets, num_actual_buckets>1, {
      bkt_counts[i*S_STRIDE] = 0;
      if (i == (num_total_buckets-1)) {
        size_t ct = 0;
        for (size_t j=0; j<num_blocks; j++) {
          size_t start = std::min(j * block_size, n);
          size_t end =  std::min(start + block_size, n);
          ct += (end - start) - counts[j*num_total_buckets+i];
        }
        bkt_counts[i*S_STRIDE] = ct;
      } else {
        size_t ct = 0;
        for (size_t j=0; j<num_blocks; j++) {
          ct += counts[j*num_total_buckets+i+1] - counts[j*num_total_buckets+i];
        }
        bkt_counts[i*S_STRIDE] = ct;
      }
    });

    array_imap<size_t> out_offs = array_imap<size_t>(num_buckets+1);
    array_imap<size_t> ht_offs = array_imap<size_t>(num_buckets+1);
    auto empty = ht.empty;
    KV* table = ht.table;

    using MO = Maybe<O>;
    MO heavy_cts_stk[128]; MO* heavy_cts;
    if (heavy) {
      if (num_heavy > 128) { heavy_cts = newA(MO, num_heavy); }
      else { heavy_cts = heavy_cts_stk; }
//      for (size_t i=0; i<num_heavy; i++) {
//        heavy_cts[i] = Maybe<O>();
//        cout << "cnt i = " << i << " = " << bkt_counts[(num_buckets+i)*S_STRIDE] << endl;
//      }
    }

    // (2) process each bucket, compute the size of each HT and scan (seq)
    ht_offs[0] = 0;
    size_t min_size = std::numeric_limits<size_t>::max();
    size_t max_size = 0;
    size_t avg_size = std::ceil(n/num_buckets);
    for (size_t i=0; i<num_buckets; i++) {
      size_t size = bkt_counts[i*S_STRIDE];
      size_t ht_size = 0;
      if (size > 0) {
        ht_size = 1 << pbbs::log2_up((intT)(size + 1));
      }
      ht_offs[i+1] = ht_offs[i] + ht_size;
      if (size < min_size) {
        min_size = size;
      }
      if (size > max_size) {
        max_size = size;
      }
    }

//    if (n > 100000) {
//      cout << "n = " << n << " min size = " << min_size << " max size = " << max_size << " avg size = " << avg_size << endl; //" k = " << gb.k << endl;
//    }

    ht.resize(ht_offs[num_buckets]);

    // (3) insert elms into per-bucket hash table (par)

    parallel_for (size_t i=0; i<num_actual_buckets; i++) {
      if (i < num_buckets) {
        size_t ht_start = ht_offs[i];
        size_t ht_size = ht_offs[i+1] - ht_start;

        size_t k = 0;
        if (ht_size > 0) {
          KV* my_ht = &(table[ht_start]);
          sequentialHT<K, V> S(my_ht, ht_size, empty);

          for (size_t j=0; j<num_blocks; j++) {
            size_t start = std::min(j * block_size, n);
            size_t end =  std::min(start + block_size, n);
            size_t ct = 0;
            size_t off = 0;
            if (i == (num_total_buckets-1)) {
              off = counts[j*num_total_buckets+i];
              ct = (end - start) - off;
            } else {
              off = counts[j*num_total_buckets+i];
              ct = counts[j*num_total_buckets+i+1] - off;
            }
            off += start;
            for (size_t k=0; k<ct; k++) {
              K a = elms[off + k];
              S.insertAdd(a);
            }
          }

          k = S.compactIntoSelf(apply_f);
        }
        out_offs[i] = k;
      } else {
        // heavy bucket
        size_t bkt_id = i - num_buckets;
        K key; bool is_set = false;
        size_t total_ct = 0;
        for (size_t j=0; j<num_blocks; j++) {
          size_t start = std::min(j * block_size, n);
          size_t end = std::min(start + block_size, n);
          size_t ct = 0;
          size_t off = 0;
          if (i == (num_total_buckets-1)) {
            off = counts[j*num_total_buckets+i];
            ct = (end - start) - off;
          } else {
            off = counts[j*num_total_buckets+i];
            ct = counts[j*num_total_buckets+i+1] - off;
          }
          off += start;
          if (!is_set && ct) {
            key = elms[off];
            is_set = true;
          }
          total_ct += ct;
        }
        assert(is_set);

        Maybe<O> value = apply_f(make_tuple(key, total_ct));
        heavy_cts[bkt_id] = value;
      }
    }

    // (4) scan
    size_t ct = 0;
    for (size_t i=0; i<num_buckets; i++) {
      size_t s = ct;
      ct += out_offs[i];
      out_offs[i] = s;
    }

    out_offs[num_buckets] = ct;
    size_t heavy_start = ct;

    if (heavy) {
      for (size_t i=0; i<num_heavy; i++) {
        if (isSome(heavy_cts[i])) {
          ct++;
        }
      }
    }
    uintT num_distinct = ct;

    O* res = newA(O, ct);

    // (5) map compacted hts to output, clear hts
    granular_for(i, 0, num_buckets, (num_buckets>1), {
      size_t o = out_offs[i];
      size_t k = out_offs[(i+1)] - o;

      size_t ht_start = ht_offs[i];
      size_t ht_size = ht_offs[i+1] - ht_start;

      if (ht_size > 0) {
        KV* my_ht = &(table[ht_start]);

        for (size_t j=0; j<k; j++) {
          res[o+j] = my_ht[j];
          my_ht[j] = empty;
        }
      }
    });

    if (heavy) {
      size_t heavy_off = 0;
      for (size_t i=0; i<num_heavy; i++) {
        auto& value = heavy_cts[i];
        if (isSome(value)) {
          res[heavy_start + heavy_off++] = getT(value);
        }
      }
    }

    free(elms); free(counts); free(bkt_counts);
    if (heavy && num_heavy > 128) { free(heavy_cts); }

    return make_pair(num_distinct, res);
  }

  template <class E, class O, class K, class V, class A, class Reduce, class Apply>
  inline pair<size_t, O*> seq_histogram_reduce(A& get_elm, size_t n, Reduce& reduce_f, Apply& apply_f, hist_table<K, V>& ht) {
    typedef tuple<K, V> KV;
    size_t pn = pbbs::log2_up((intT)(n + 1));
    size_t rs = 1L << pn;
    ht.resize(rs);
    sequentialHT<K, V> S(ht.table, n, 1.0f, ht.empty);
    for (size_t i=0; i<n; i++) {
      E a = get_elm(i);
      reduce_f(S, a);
    }
    O* out = newA(O, n);
    size_t k = S.compactInto(apply_f, out);
    return make_pair(k, out);
  }

  // Issue: want to make the type that's count-sort'd independent of K,V
  // A : <E> elms
  // B : inmap<keys> (numeric so that we can hash)
  // E : type of intermediate elements (what we count sort)
  // Q : reduction (seqHT<uintE, E>&, E&) -> void
  // F : tuple<K, Elm> -> Maybe<uintE>
  template <class E, class O, class K, class V, class A, class B, class Reduce, class Apply>
  inline pair<size_t, O*> histogram_reduce(A& get_elm, B& get_key, size_t n, Reduce& reduce_f, Apply& apply_f, hist_table<K, V>& ht) {
    typedef tuple<K, V> KV;

    int nworkers = getWorkers();

    if (n < _hist_seq_threshold || nworkers == 1) {
      auto r = seq_histogram_reduce<E, O>(get_elm, n, reduce_f, apply_f, ht);
      return r;
    }

    size_t sqrt = (size_t) ceil(pow(n,0.5));
    size_t num_buckets = (size_t) (n < 20000000) ? (sqrt/5) : sqrt;

    num_buckets = std::max(1 << log2_up(num_buckets), 1);
    num_buckets = min(num_buckets, _hist_max_buckets);
    size_t bits = log2_up(num_buckets);

//    auto gb = get_bucket<K, B>(get_key, n, bits);
    // (1) count-sort based on bucket
    // TODO: add back contention detection
    size_t low_mask = ~((size_t)15);
    size_t bucket_mask = num_buckets - 1;
    auto gb = [&] (uintE i) {
      return pbbs::hash64(get_key[i] & low_mask) & bucket_mask;
    };

    auto p = _count_sort<int16_t, size_t, E>(get_elm, gb, n, (uintE)num_buckets);

    auto elms = std::get<0>(p); // count-sort'd
    // laid out as num_buckets (row), blocks (col)
    size_t* counts = std::get<1>(p);
    size_t num_blocks = std::get<2>(p);
    size_t block_size = ((n-1)/num_blocks) + 1;

#define S_STRIDE 64
    size_t* bkt_counts = new_array_no_init<size_t>(num_buckets*S_STRIDE);
    granular_for(i, 0, num_buckets, num_buckets>1, {
      bkt_counts[i*S_STRIDE] = 0;
      if (i == (num_buckets-1)) {
        size_t ct = 0;
        for (size_t j=0; j<num_blocks; j++) {
          size_t start = std::min(j * block_size, n);
          size_t end =  std::min(start + block_size, n);
          ct += (end - start) - counts[j*num_buckets+i];
        }
        bkt_counts[i*S_STRIDE] = ct;
      } else {
        size_t ct = 0;
        for (size_t j=0; j<num_blocks; j++) {
          ct += counts[j*num_buckets+i+1] - counts[j*num_buckets+i];
        }
        bkt_counts[i*S_STRIDE] = ct;
      }
    });

    array_imap<size_t> out_offs = array_imap<size_t>(num_buckets+1);
    array_imap<size_t> ht_offs = array_imap<size_t>(num_buckets+1);

    // (2) process each bucket, compute the size of each HT and scan (seq)
    ht_offs[0] = 0;
    for (size_t i=0; i<num_buckets; i++) {
      size_t size = bkt_counts[i*S_STRIDE];
      size_t ht_size = 0;
      if (size > 0) {
        ht_size = 1 << pbbs::log2_up((intT)(size + 1));
      }
      ht_offs[i+1] = ht_offs[i] + ht_size;
    }

    ht.resize(ht_offs[num_buckets]);
    O* out = newA(O, ht_offs[num_buckets]);

    // (3) insert elms into per-bucket hash table (par)
    parallel_for(size_t i=0; i<num_buckets; i++) {
      size_t ht_start = ht_offs[i];
      size_t ht_size = ht_offs[i+1] - ht_start;

      size_t k = 0;
      KV* table = ht.table;
      if (ht_size > 0) {
        KV* my_ht = &(table[ht_start]);
        sequentialHT<K, V> S(my_ht, ht_size, ht.empty);

        O* my_out = &(out[ht_start]);

        for (size_t j=0; j<num_blocks; j++) {
          size_t start = std::min(j * block_size, n);
          size_t end =  std::min(start + block_size, n);
          size_t ct = 0;
          size_t off = 0;
          if (i == (num_buckets-1)) {
            off = counts[j*num_buckets+i];
            ct = (end - start) - off;
          } else {
            off = counts[j*num_buckets+i];
            ct = counts[j*num_buckets+i+1] - off;
          }
          off += start;
          for (size_t k=0; k<ct; k++) {
            E a = elms[off + k];
            reduce_f(S, a);
          }
        }

        k = S.compactInto(apply_f, my_out);
      }
      out_offs[i] = k;
    }

    // (4) scan
    size_t ct = 0;
    for (size_t i=0; i<num_buckets; i++) {
      size_t s = ct;
      ct += out_offs[i];
      out_offs[i] = s;
    }
    out_offs[num_buckets] = ct;
    uintT num_distinct = ct;

    O* res = newA(O, ct);

    // (5) map compacted hts to output, clear hts
    granular_for(i, 0, num_buckets, (num_buckets>1), {
      size_t o = out_offs[i];
      size_t k = out_offs[(i+1)] - o;

      size_t ht_start = ht_offs[i];
      size_t ht_size = ht_offs[i+1] - ht_start;

      if (ht_size > 0) {
        O* my_out = &(out[ht_start]);

        for (size_t j=0; j<k; j++) {
          res[o+j] = my_out[j];
        }
      }
    });

    free(elms); free(counts); free(bkt_counts); free(out);
    return make_pair(num_distinct, res);
  }

//  template <class E, class A, class F, class Timer>
//  inline pair<size_t, tuple<E, E>*> histogram(A& get_elm, size_t n, F& f, hist_table<E, E>& ht, Timer& t) {
//    auto q = [] (sequentialHT<E, E>& S, E v) -> void { S.insertAdd(v); };
//    return histogram_reduce<E, tuple<E, E>>(get_elm, get_elm, n, q, f, ht, t);
//  }

}
