/*  
 * Copyright (c) 2013 Shanghai Jiao Tong University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/powerlyra.html
 *
 *
 * 2015.06  re-implement batch ingress (coordinated vertex-cut), w/o multithreading
 *
 */

 /**  
  * Copyright (c) 2009 Carnegie Mellon University. 
  *     All rights reserved.
  *
  *  Licensed under the Apache License, Version 2.0 (the "License");
  *  you may not use this file except in compliance with the License.
  *  You may obtain a copy of the License at
  *
  *      http://www.apache.org/licenses/LICENSE-2.0
  *
  *  Unless required by applicable law or agreed to in writing,
  *  software distributed under the License is distributed on an "AS
  *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
  *  express or implied.  See the License for the specific language
  *  governing permissions and limitations under the License.
  *
  * For more about this software visit:
  *
  *      http://www.graphlab.ml.cmu.edu
  *
  */
 
#ifndef GRAPHLAB_DISTRIBUTED_BATCH_INGRESS_HPP
#define GRAPHLAB_DISTRIBUTED_BATCH_INGRESS_HPP
 
#include <boost/unordered_set.hpp>
#include <graphlab/graph/graph_basic_types.hpp>
#include <graphlab/graph/graph_hash.hpp>
#include <graphlab/graph/ingress/distributed_ingress_base.hpp>
#include <graphlab/graph/distributed_graph.hpp>
#include <graphlab/rpc/buffered_exchange.hpp>
#include <graphlab/rpc/distributed_event_log.hpp>
#include <graphlab/util/dense_bitset.hpp>
#include <graphlab/macros_def.hpp>
 
 namespace graphlab {
   template<typename VertexData, typename EdgeData>
     class distributed_graph;
 
   template<typename VertexData, typename EdgeData>
   class distributed_batch_ingress : 
     public distributed_ingress_base<VertexData, EdgeData> {
   public:
     typedef distributed_graph<VertexData, EdgeData> graph_type;
     /// The type of the vertex data stored in the graph 
     typedef VertexData vertex_data_type;
     /// The type of the edge data stored in the graph 
     typedef EdgeData edge_data_type;
 
     typedef typename graph_type::vertex_record vertex_record;
     typedef typename graph_type::mirror_type mirror_type;
 
     dc_dist_object<distributed_batch_ingress> rpc;
     typedef distributed_ingress_base<VertexData, EdgeData> base_type;
 
     mutex local_graph_lock;
     mutex lvid2record_lock;
 
     typedef fixed_dense_bitset<RPC_MAX_N_PROCS> bin_counts_type;
 
     /** Type of the degree hash table: 
      * a map from vertex id to a bitset of length num_procs. */
     typedef typename boost::unordered_map<vertex_id_type, bin_counts_type> 
     dht_degree_table_type;
 
     /** distributed hash table stored on local machine */ 
     boost::unordered_map<vertex_id_type, bin_counts_type > dht_degree_table;
 
     /** The map from vertex id to its DHT entry.
      * Must be called with a readlock acquired on dht_degree_table_lock. */
     size_t vid_to_dht_entry_with_readlock(vertex_id_type vid) {
       if (dht_degree_table.count(vid) == 0) {
         dht_degree_table_lock.unlock();
         dht_degree_table_lock.writelock();
         dht_degree_table[vid].clear(); 
         dht_degree_table_lock.unlock();
         dht_degree_table_lock.readlock();
       }
       return vid;
     }
     rwlock dht_degree_table_lock;
 
     /** Local minibatch buffer */
     /** Number of edges in the current buffer. */
     size_t num_edges; 
     /** Capacity of the local buffer. */
     size_t bufsize; 
     std::vector<std::pair<vertex_id_type, vertex_id_type> > edgesend;
     std::vector<EdgeData> edatasend;
     mutex edgesend_lock;
     /** A set of vertex in the current batch requiring query the DHT. */
     std::vector<boost::unordered_set<vertex_id_type> > query_set;
     /** The map from proc_id to num_edges on that proc */
     std::vector<size_t> proc_num_edges;
 
     DECLARE_TRACER(batch_ingress_add_edge);
     DECLARE_TRACER(batch_ingress_add_edges);
     DECLARE_TRACER(batch_ingress_compute_assignments);
     DECLARE_TRACER(batch_ingress_request_degree_table);
     DECLARE_TRACER(batch_ingress_get_degree_table);
     DECLARE_TRACER(batch_ingress_update_degree_table);
 
     /** Ingress tratis. */
     bool usehash;
     bool userecent; 
 
   public:
     distributed_batch_ingress(distributed_control& dc, graph_type& graph, 
         size_t bufsize = 50000, bool usehash = false, bool userecent = false) :
       base_type(dc, graph), rpc(dc, this),
       num_edges(0), bufsize(bufsize), query_set(dc.numprocs()),
       proc_num_edges(dc.numprocs()), usehash(usehash), userecent(userecent) { 
        rpc.barrier(); 
 
       INITIALIZE_TRACER(batch_ingress_add_edge, "Time spent in add edge");
       INITIALIZE_TRACER(batch_ingress_add_edges, "Time spent in add block edges" );
       INITIALIZE_TRACER(batch_ingress_compute_assignments, "Time spent in compute assignment");
       INITIALIZE_TRACER(batch_ingress_request_degree_table, "Time spent in requesting assignment");
       INITIALIZE_TRACER(batch_ingress_get_degree_table, "Time spent in retrieve degree table");
       INITIALIZE_TRACER(batch_ingress_update_degree_table, "Time spent in update degree table");
      }
 
     /** Adds an edge to the batch ingress buffer, and updates the query set. */
     void add_edge(vertex_id_type source, vertex_id_type target, const EdgeData& edata) {
       BEGIN_TRACEPOINT(batch_ingress_add_edge);
       edgesend_lock.lock();
       ASSERT_LT(edgesend.size(), bufsize);
       edgesend.push_back(std::make_pair(source, target)); 
       edatasend.push_back(edata);        
       query_set[graph_hash::hash_vertex(source) % rpc.numprocs()].insert(source);
       query_set[graph_hash::hash_vertex(target) % rpc.numprocs()].insert(target);
       ++num_edges;
       edgesend_lock.unlock();
       END_TRACEPOINT(batch_ingress_add_edge);
       if (is_full()) flush();
     } // end of add_edge
 
     /** Flush the buffer and call base finalize. */; 
     void finalize() { 
       graphlab::timer ti;
       rpc.full_barrier();
       flush(); 
 
       //rpc.full_barrier();
       //base_type::finalize();
       base_finalize();
       rpc.full_barrier();
 
       if(rpc.procid() == 0)
         logstream(LOG_EMPH) << "batch finalizing graph done. (" 
                             << ti.current_time() 
                             << " secs)" 
                             << std::endl;
     } // end of finalize
 
     void base_finalize() {
 
       graphlab::timer mytimer; mytimer.start();
 
       rpc.full_barrier();
 
       if (rpc.procid() == 0) {
         logstream(LOG_EMPH) << "Finalizing Graph ... " 
                             << " #vertices=" <<  base_type::graph.local_graph.num_vertices()
                             << " #edges=" <<  base_type::graph.local_graph.num_edges()
                             << std::endl;
       }
 
       typedef typename hopscotch_map<vertex_id_type, lvid_type>::value_type
         vid2lvid_pair_type;
 
       typedef typename buffered_exchange<typename base_type::vertex_buffer_record>::buffer_type 
         vertex_buffer_type;
 
 
       /**************************************************************************/
       /*                                                                        */
       /*                       flush any additional data                        */
       /*                                                                        */
       /**************************************************************************/
 
       ASSERT_EQ(base_type::graph.vid2lvid.size(), 
                 base_type::graph.local_graph.num_vertices());
       
       // Finalize local graph
       logstream(LOG_INFO) << "Graph Finalize: finalizing local graph." 
                           << std::endl;
       base_type::graph.local_graph.finalize();
       logstream(LOG_INFO) << "Local graph info: " << std::endl
                           << "\t nverts: " << base_type::graph.local_graph.num_vertices()
                           << std::endl
                           << "\t nedges: " << base_type::graph.local_graph.num_edges()
                           << std::endl;
       
       if(rpc.procid() == 0) {
         memory_info::log_usage("Finished finalizing local graph."); 
         // debug
         // std::cout << graph.local_graph << std::endl;
       }
 
       /**************************************************************************/
       /*                                                                        */
       /*             receive and add vertex data to masters                     */
       /*                                                                        */
       /**************************************************************************/
       // Setup the map containing all the vertices being negotiated by this machine
       { // Receive any vertex data sent by other machines
         base_type::vertex_exchange.flush();
         vertex_buffer_type vertex_buffer; procid_t sending_proc(-1);
         while(base_type::vertex_exchange.recv(sending_proc, vertex_buffer)) {
           foreach(const typename base_type::vertex_buffer_record& rec, vertex_buffer) {
             lvid_type lvid(-1);
 
             typedef typename hopscotch_map<vertex_id_type, lvid_type>::iterator
               vid2lvid_iter;
             vid2lvid_iter iter;
 
             iter = base_type::graph.vid2lvid.find(rec.vid);
             if (iter == base_type::graph.vid2lvid.end()) {
               lvid = base_type::graph.vid2lvid.size();
               base_type::graph.vid2lvid[rec.vid] = lvid;
             } else {
               lvid = iter->second;
             }
           
             if (vertex_combine_strategy && lvid < base_type::graph.num_local_vertices()) {
                vertex_combine_strategy(base_type::graph.l_vertex(lvid).data(), rec.vdata);
             } else {
               base_type::graph.local_graph.add_vertex(lvid, rec.vdata);
             }
           }
         }
          base_type::vertex_exchange.clear();
         if(rpc.procid() == 0)         
           memory_info::log_usage("Finished adding vertex data");
       } // end of loop to populate vrecmap
 
 
 
       /**************************************************************************/
       /*                                                                        */
       /*        assign vertex data and allocate vertex (meta)data  space        */
       /*                                                                        */
       /**************************************************************************/
       { // Determine masters for all negotiated vertices
         const size_t local_nverts = base_type::graph.vid2lvid.size();
         base_type::graph.lvid2record.reserve(local_nverts);
         base_type::graph.lvid2record.resize(local_nverts);
         base_type::graph.local_graph.resize(local_nverts);
         foreach(const vid2lvid_pair_type& pair, base_type::graph.vid2lvid) {
             vertex_record& vrec = base_type::graph.lvid2record[pair.second];
             vrec.gvid = pair.first;
             vrec.owner = graph_hash::hash_vertex(pair.first) % rpc.numprocs();
         }
         ASSERT_EQ(local_nverts, base_type::graph.local_graph.num_vertices());
         ASSERT_EQ(base_type::graph.lvid2record.size(), base_type::graph.local_graph.num_vertices());
         if(rpc.procid() == 0)       
           memory_info::log_usage("Finished allocating lvid2record");
       }
 
       /**************************************************************************/
       /*                                                                        */
       /*                          master handshake                              */
       /*                                                                        */
       /**************************************************************************/
       {
#ifdef _OPENMP
         buffered_exchange<vertex_id_type> vid_buffer(rpc.dc(), omp_get_max_threads());
#else
         buffered_exchange<vertex_id_type> vid_buffer(rpc.dc());
#endif
 
#ifdef _OPENMP
#pragma omp parallel for
#endif
         // send not owned vids to their master
         for (lvid_type i = 0; i < base_type::graph.lvid2record.size(); ++i) {
           procid_t master = base_type::graph.lvid2record[i].owner;
           if (master != rpc.procid())
#ifdef _OPENMP
             vid_buffer.send(master, base_type::graph.lvid2record[i].gvid, omp_get_thread_num());
#else
             vid_buffer.send(master, base_type::graph.lvid2record[i].gvid);
#endif
         }
         vid_buffer.flush();
         rpc.barrier();
 
         // receive all vids owned by me
         mutex flying_vids_lock;
         boost::unordered_map<vertex_id_type, mirror_type> flying_vids;
#ifdef _OPENMP
#pragma omp parallel
#endif
         {
           typename buffered_exchange<vertex_id_type>::buffer_type buffer;
           procid_t recvid = -1;
           while(vid_buffer.recv(recvid, buffer)) {
             foreach(const vertex_id_type vid, buffer) {
               if (base_type::graph.vid2lvid.find(vid) == base_type::graph.vid2lvid.end()) {
                 flying_vids_lock.lock();
                 mirror_type& mirrors = flying_vids[vid];
                 mirrors.set_bit(recvid);
                 flying_vids_lock.unlock();
               } else {
                 lvid_type lvid = base_type::graph.vid2lvid[vid];
                 base_type::graph.lvid2record[lvid]._mirrors.set_bit(recvid);
               }
             }
           }
         }
 
         vid_buffer.clear();
 
         if (!flying_vids.empty()) {
           logstream(LOG_INFO) << "Graph Finalize: #flying-own-nverts="
                               << flying_vids.size() 
                               << std::endl;
 
           // reallocate spaces for the flying vertices. 
           size_t vsize_old = base_type::graph.lvid2record.size();
           size_t vsize_new = vsize_old + flying_vids.size();
           base_type::graph.lvid2record.resize(vsize_new);
           base_type::graph.local_graph.resize(vsize_new);
           for (typename boost::unordered_map<vertex_id_type, mirror_type>::iterator it = flying_vids.begin();
                it != flying_vids.end(); ++it) {
             lvid_type lvid = base_type::graph.vid2lvid.size();
             vertex_id_type gvid = it->first;
             base_type::graph.lvid2record[lvid].owner = rpc.procid();
             base_type::graph.lvid2record[lvid].gvid = gvid;
             base_type::graph.lvid2record[lvid]._mirrors= it->second;
             base_type::graph.vid2lvid[gvid] = lvid;
             //std::cout << "proc " << rpc.procid() << " flying vertex gvid=" << gvid << " lvid=" << lvid << std::endl;*/
           }
         }
       } // end of master handshake
 
 
       /**************************************************************************/
       /*                                                                        */
       /*              synchronize vertex data and meta information              */
       /*                                                                        */
       /**************************************************************************/
       {
         // construct the vertex set of changed vertices
         
         // Fast pass for first time finalize;
         vertex_set changed_vset(true);
         graphlab::graph_gather_apply<graph_type, typename base_type::vertex_negotiator_record> 
             vrecord_sync_gas(base_type::graph, 
                              boost::bind(&distributed_batch_ingress::finalize_gather, this, _1, _2), 
                              boost::bind(&distributed_batch_ingress::finalize_apply, this, _1, _2, _3));
         vrecord_sync_gas.exec(changed_vset);
 
         if(rpc.procid() == 0)       
           memory_info::log_usage("Finished synchronizing vertex (meta)data");
       }
 
       base_type::exchange_global_info(false);
       logstream(LOG_INFO) << "Base finalized in " << mytimer.current_time() 
                           << " secs" << std::endl;
     }
 
   private:
     boost::function<void(vertex_data_type&, const vertex_data_type&)> vertex_combine_strategy;
 
     /**
      * \brief Gather the vertex distributed meta data.
      */
     typename base_type::vertex_negotiator_record finalize_gather(lvid_type& lvid, graph_type& graph) {
       typename base_type::vertex_negotiator_record accum;
       accum.num_in_edges = base_type::graph.local_graph.num_in_edges(lvid);
       accum.num_out_edges = base_type::graph.local_graph.num_out_edges(lvid);
       if (graph.l_is_master(lvid)) {
         accum.has_data = true;
         accum.vdata = base_type::graph.l_vertex(lvid).data();
         accum.mirrors = base_type::graph.lvid2record[lvid]._mirrors;
       } 
       return accum;
     }
 
     /**
      * \brief Update the vertex datastructures with the gathered vertex metadata.  
      */
     void finalize_apply(lvid_type lvid, const typename base_type::vertex_negotiator_record& accum, graph_type& graph) {
       typename graph_type::vertex_record& vrec = base_type::graph.lvid2record[lvid];
       vrec.num_in_edges = accum.num_in_edges;
       vrec.num_out_edges = accum.num_out_edges;
       base_type::graph.l_vertex(lvid).data() = accum.vdata;
       vrec._mirrors = accum.mirrors;
     }
 
 
 
 
     // HELPER ROUTINES =======================================================>    
     /** Add edges in block to the local current graph. */
     void batch_add_edges(const std::vector<vertex_id_type>& source_arr, 
         const std::vector<vertex_id_type>& target_arr, 
         const std::vector<EdgeData>& edata_arr) {
 
       BEGIN_TRACEPOINT(batch_ingress_add_edges);
       ASSERT_TRUE((source_arr.size() == target_arr.size())
           && (source_arr.size() == edata_arr.size())); 
       if (source_arr.size() == 0) return;
 
       std::vector<lvid_type> local_source_arr; 
       local_source_arr.reserve(source_arr.size());
       std::vector<lvid_type> local_target_arr;
       local_target_arr.reserve(source_arr.size());
 
       // The map from vertex_id to its degree on this proc.
       std::vector<std::vector<vertex_id_type> > local_degree_count(rpc.numprocs());
 
       lvid_type max_lvid = 0;
 
       lvid2record_lock.lock();
       // Lock and update the lvid2record.
       for (size_t i = 0; i < source_arr.size(); ++i) {
         vertex_id_type source = source_arr[i];
         vertex_id_type target = target_arr[i]; 
         lvid_type lvid_source(-1);
         lvid_type lvid_target(-1);
         // typedef typename boost::unordered_map<vertex_id_type, lvid_type>::iterator 
           // vid2lvid_iter;
         typedef typename hopscotch_map<vertex_id_type, lvid_type>::iterator
           vid2lvid_iter;
         vid2lvid_iter iter;
 
           iter = base_type::graph.vid2lvid.find(source);
           if (iter == base_type::graph.vid2lvid.end()) {
             lvid_source = base_type::graph.vid2lvid.size();
             base_type::graph.vid2lvid[source]=lvid_source;
             base_type::graph.lvid2record.push_back(vertex_record(source));
           } else {
             lvid_source = iter->second;
           }
 
           iter = base_type::graph.vid2lvid.find(target);
           if (iter == base_type::graph.vid2lvid.end()) {
             lvid_target = base_type::graph.vid2lvid.size();
             base_type::graph.vid2lvid[target]=lvid_target;
             base_type::graph.lvid2record.push_back(vertex_record(target));
           } else {
             lvid_target = iter->second;
           }
 
         local_source_arr.push_back(lvid_source);
         local_target_arr.push_back(lvid_target);
         max_lvid = std::max(std::max(lvid_source, lvid_target), 
             max_lvid);
 
         local_degree_count[graph_hash::hash_vertex(source) % rpc.numprocs()].push_back(source);
         local_degree_count[graph_hash::hash_vertex(target) % rpc.numprocs()].push_back(target);
       }
       lvid2record_lock.unlock();
 
       // Send out local_degree count.
       for (size_t i = 0; i < rpc.numprocs(); ++i) {
         if (i != rpc.procid()) {
           rpc.remote_call(i, 
                           &distributed_batch_ingress::block_add_degree_counts, 
                           rpc.procid(),
                           local_degree_count[i]);
         } else {
           block_add_degree_counts(rpc.procid(), local_degree_count[i]);
         }
         local_degree_count[i].clear();
       }
 
       // Lock and add edges to local graph.
       local_graph_lock.lock();
       if (max_lvid >= base_type::graph.local_graph.num_vertices()) {
         //std::cout << rpc.procid() << ": " << max_lvid << std::endl;
         base_type::graph.local_graph.resize(max_lvid + 1);
       }
       base_type::graph.local_graph.add_edges(local_source_arr, 
                                              local_target_arr, edata_arr);
       local_graph_lock.unlock();
  
       END_TRACEPOINT(batch_ingress_add_edges);
     } // end of add edges
 
     /** Updates the local part of the distributed table. */
     void block_add_degree_counts (procid_t pid, std::vector<vertex_id_type>& whohas) {
       BEGIN_TRACEPOINT(batch_ingress_update_degree_table);
       dht_degree_table_lock.readlock();
       foreach (vertex_id_type& vid, whohas) {
         size_t idx = vid_to_dht_entry_with_readlock(vid);
         dht_degree_table[idx].set_bit_unsync(pid);
       }
       dht_degree_table_lock.unlock();
       END_TRACEPOINT(batch_ingress_update_degree_table);
     }
 
     /** Returns the degree counts by querying the distributed table. */
     dht_degree_table_type 
     block_get_degree_table(const boost::unordered_set<vertex_id_type>& vid_query) {
       BEGIN_TRACEPOINT(batch_ingress_get_degree_table);
       dht_degree_table_type answer;
       dht_degree_table_lock.readlock();
       foreach (vertex_id_type qvid, vid_query) {
         answer[qvid] = dht_degree_table[vid_to_dht_entry_with_readlock(qvid)]; 
       }
       dht_degree_table_lock.unlock();
       END_TRACEPOINT(batch_ingress_get_degree_table);
       return answer;
     }  // end of block get degree table
 
 
     /** Assign edges in the buffer greedily using the recent query of DHT. */
    void assign_edges(std::vector<std::vector<vertex_id_type> >& proc_src,
                      std::vector<std::vector<vertex_id_type> >& proc_dst,
                      std::vector<std::vector<EdgeData> >& proc_edata) {
      ASSERT_EQ(num_edges, edgesend.size());
 
      edgesend_lock.lock();
      
      if (num_edges == 0) {
       edgesend_lock.unlock();
       return;
      }
      BEGIN_TRACEPOINT(batch_ingress_request_degree_table);
      std::vector<dht_degree_table_type> degree_table(rpc.numprocs());
      
      // Query the DHT.
      for (size_t i = 0; i < rpc.numprocs(); ++i) {
        if (i == rpc.procid()) {
          degree_table[i] = block_get_degree_table(query_set[i]);
        } else {
          degree_table[i] = 
            rpc.remote_request(i, 
                &distributed_batch_ingress::block_get_degree_table,
                query_set[i]);
        }
        query_set[i].clear();
      }
      END_TRACEPOINT(batch_ingress_request_degree_table);
 
      // Make assigment.
      for (size_t i = 0; i < num_edges; ++i) {
        std::pair<vertex_id_type, vertex_id_type>& e = 
          edgesend[i];
 
        BEGIN_TRACEPOINT(batch_ingress_compute_assignments);
        size_t src_proc = graph_hash::hash_vertex(e.first) % rpc.numprocs();
        size_t dst_proc = graph_hash::hash_vertex(e.second) % rpc.numprocs();
        bin_counts_type& src_degree = degree_table[src_proc][e.first];
        bin_counts_type& dst_degree = degree_table[dst_proc][e.second];
        procid_t proc = base_type::edge_decision.edge_to_proc_greedy(e.first, e.second, 
            src_degree, dst_degree, proc_num_edges, usehash, userecent);
        END_TRACEPOINT(batch_ingress_compute_assignments);
 
        ASSERT_LT(proc, proc_src.size());
        proc_src[proc].push_back(e.first);
        proc_dst[proc].push_back(e.second);
        proc_edata[proc].push_back(edatasend[i]);
      }
 
      // Clear the sending buffer.
      edgesend.clear();
      edatasend.clear();
      edgesend_lock.unlock();
    } // end assign edge
 
     /** Flushes all edges in the buffer. */
     void flush() {
       std::vector< std::vector<vertex_id_type> > proc_src(rpc.numprocs());
       std::vector< std::vector<vertex_id_type> > proc_dst(rpc.numprocs());
       std::vector< std::vector<EdgeData> > proc_edata(rpc.numprocs());
       assign_edges(proc_src, proc_dst, proc_edata);
       for (size_t i = 0; i < proc_src.size(); ++i) {
         if (proc_src[i].size() == 0) 
           continue;
         if (i == rpc.procid()) {
           batch_add_edges(proc_src[i], proc_dst[i], proc_edata[i]);
           num_edges -= proc_src[i].size();
         } else {
           rpc.remote_call(i, &distributed_batch_ingress::batch_add_edges,
               proc_src[i], proc_dst[i], proc_edata[i]);
           num_edges -= proc_src[i].size();
         } // end if
       } // end for
     } // end flush
 
     /** Returns the number of edges  in the buffer. */
     size_t size() { return num_edges; }
 
     /** Returns whether the buffer is full. */
     bool is_full() { return size() >= bufsize; }
   }; // end of distributed_batch_ingress
 
 }; // end of namespace graphlab
#include <graphlab/macros_undef.hpp>
 
 
#endif
