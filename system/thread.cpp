#include "global.h"
#include "manager.h"
#include "thread.h"
#include "txn.h"
#include "wl.h"
#include "query.h"
#include "plock.h"
#include "occ.h"
#include "vll.h"
#include "ycsb_query.h"
#include "tpcc_query.h"
#include "mem_alloc.h"
#include "test.h"
#include "transport.h"
#include "remote_query.h"
#include "math.h"
#include "specex.h"

void thread_t::init(uint64_t thd_id, uint64_t node_id, workload * workload) {
	_thd_id = thd_id;
	_node_id = node_id;
	_wl = workload;
}

uint64_t thread_t::get_thd_id() { return _thd_id; }
uint64_t thread_t::get_node_id() { return _node_id; }
uint64_t thread_t::get_host_cid() {	return _host_cid; }
void thread_t::set_host_cid(uint64_t cid) { _host_cid = cid; }
uint64_t thread_t::get_cur_cid() { return _cur_cid; }
void thread_t::set_cur_cid(uint64_t cid) {_cur_cid = cid; }

RC thread_t::run_remote() {
	printf("Run_remote %ld:%ld\n",_node_id, _thd_id);
  fflush(stdout);
#if !NOGRAPHITE
	_thd_id = CarbonGetTileId();
#endif
	if (warmup_finish) {
		mem_allocator.register_thread(_thd_id);
	}

	base_query * m_query = NULL;

	pthread_barrier_wait( &warmup_bar );
	stats.init(get_thd_id());
	// Send start msg to all nodes; wait for rsp from all nodes before continuing.
	//#if !TPORT_TYPE_IPC
	int rsp_cnt = g_node_cnt + g_client_node_cnt - 1;
	int done_cnt = g_node_cnt + g_client_node_cnt - 1;
#if CC_ALG == CALVIN
	rsp_cnt++;	// Account for sequencer node
#endif
	while(rsp_cnt > 0) {
		m_query = (base_query *) tport_man.recv_msg();
		if(m_query != NULL && m_query->rtype == INIT_DONE) {
			rsp_cnt --;
		} else if(m_query != NULL) {
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
			work_queue.add_query(GET_PART_ID_IDX(m_query->active_part),m_query);
#else
			work_queue.add_query(0,m_query);
#endif
      /*
      assert(m_query->part_cnt > 0 || m_query->part_num > 0);
      if(m_query->part_num > 0)
			  work_queue.add_query(m_query->part_to_access[0]/g_node_cnt,m_query);
      else if(m_query->part_cnt > 0)
			  work_queue.add_query(m_query->parts[0]/g_node_cnt,m_query);
        */
		}
	}
	//#endif
	pthread_barrier_wait( &warmup_bar );
	printf("Run_remote %ld:%ld\n",_node_id, _thd_id);
  fflush(stdout);

	myrand rdm;
	rdm.init(get_thd_id());
	RC rc = RCOK;
	assert (rc == RCOK);

#if !NOGRAPHITE
	if (warmup_finish) {
		CarbonEnableModelsBarrier(&enable_barrier);
	}
#endif

	uint64_t run_starttime = get_sys_clock();
	ts_t rq_time = get_sys_clock();

	while (true) {
		m_query = (base_query *) tport_man.recv_msg();
		if( m_query != NULL ) { 
			rq_time = get_sys_clock();
      if(m_query->rtype == EXP_DONE) {
        done_cnt--;
        continue;
      }
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
			work_queue.add_query(GET_PART_ID_IDX(m_query->active_part),m_query);
#else
			work_queue.add_query(0,m_query);
#endif
      /*
      assert(m_query->part_cnt > 0 || m_query->part_num > 0);
      if(m_query->part_num > 0)
			  work_queue.add_query(m_query->part_to_access[0]/g_node_cnt,m_query);
      else if(m_query->part_cnt > 0)
			  work_queue.add_query(m_query->parts[0]/g_node_cnt,m_query);
        */
		}

		ts_t tend = get_sys_clock();
		if (warmup_finish && ((tend - run_starttime >= DONE_TIMER) || (_wl->sim_done && (  ((tend - rq_time) > MSG_TIMEOUT))))) {
			if( !ATOM_CAS(_wl->sim_timeout, false, true) )
				assert( _wl->sim_timeout);
		}

    if(warmup_finish && done_cnt == 0) {
			if( !ATOM_CAS(_wl->sim_timeout, false, true) )
				assert( _wl->sim_timeout);
			if( !ATOM_CAS(_wl->sim_done, false, true) )
				assert( _wl->sim_done);
    }

		if ((_wl->sim_done && _wl->sim_timeout) || (tend - run_starttime >= DONE_TIMER)) {
#if !NOGRAPHITE
			CarbonDisableModelsBarrier(&enable_barrier);
#endif

			return FINISH;
		}

	}

}

RC thread_t::run() {
	printf("Run %ld:%ld\n",_node_id, _thd_id);
  fflush(stdout);
#if !NOGRAPHITE
	_thd_id = CarbonGetTileId();
#endif
	if (warmup_finish) {
		mem_allocator.register_thread(_thd_id);
	}
	pthread_barrier_wait( &warmup_bar );
	stats.init(get_thd_id());

	//#if !TPORT_TYPE_IPC
	if( _thd_id == 0) {
		uint64_t total_nodes = g_node_cnt + g_client_node_cnt;
#if CC_ALG == CALVIN
		total_nodes++;
#endif
		for(uint64_t i = 0; i < total_nodes; i++) {
			if(i != g_node_id) {
				rem_qry_man.send_init_done(i);
			}
		}
	}
	//#endif
	pthread_barrier_wait( &warmup_bar );
	//sleep(4);
	printf("Run %ld:%ld\n",_node_id, _thd_id);
  fflush(stdout);

	myrand rdm;
	rdm.init(get_thd_id());
	RC rc = RCOK;
#if CC_ALG == HSTORE|| CC_ALG == HSTORE_SPEC
	RC rc2 = RCOK;
#endif
	txn_man * m_txn;
	rc = _wl->get_txn_man(m_txn, this);
	assert (rc == RCOK);
	glob_manager.set_txn_man(m_txn);

#if !NOGRAPHITE
	if (warmup_finish) {
		CarbonEnableModelsBarrier(&enable_barrier);
	}
#endif

	base_query * m_query = NULL;
	base_query * tmp_query = NULL;
	//base_query * next_query = NULL;
	uint64_t txn_cnt = 0;
	uint64_t txn_st_cnt = 0;
	uint64_t thd_txn_id = 0;
	uint64_t starttime;
	uint64_t stoptime = 0;
	uint64_t timespan;
	uint64_t ttime;
	uint64_t last_waittime = 0;
	uint64_t last_rwaittime = 0;
	uint64_t outstanding_waits = 0;
	uint64_t outstanding_rwaits = 0;
	int rsp_cnt;
  uint64_t debug1;
  RemReqType debug2;

	uint64_t run_starttime = get_sys_clock();
	uint64_t prog_time = run_starttime;
    bool txns_completed = false;

	while(true) {

		// FIXME: Susceptible to race conditions, but will hopefully even out eventually...
		// Put here so first time through, threads will populate the work queue with new txns.
		// if(txn_pool.inflight_cnt < g_inflight_max && txn_st_cnt < MAX_TXN_PER_PART/g_thread_cnt) {
		//    // Fetch new txn from query queue and add to work queue
		//    ATOM_ADD(txn_st_cnt,1);
		//    ATOM_ADD(txn_pool.inflight_cnt,1);
		//		  m_query = query_queue.get_next_query( _thd_id );
		//    work_queue.add_query(m_query);
		//}

		if(get_thd_id() == 0) {
            uint64_t now_time = get_sys_clock();
            if (now_time - prog_time >= PROG_TIMER) {
			    prog_time = now_time;
			    SET_STATS(get_thd_id(), tot_run_time, prog_time - run_starttime); 

			    stats.print(true);
            }
            if (!txns_completed && stats.get_txn_cnts() >= MAX_TXN_PER_PART) {
                assert (stats._stats[get_thd_id()]->finish_time == 0);
                txns_completed = true;
                printf("Setting final finish time\n");
                SET_STATS(get_thd_id(), finish_time, now_time - run_starttime);
                fflush(stdout);
            }
		}
		while(!work_queue.poll_next_query(get_thd_id()) && !abort_queue.poll_abort(get_thd_id()) && !_wl->sim_done && !_wl->sim_timeout) { 
#if CC_ALG == HSTORE_SPEC
      txn_pool.spec_next(_thd_id);
#endif
      // FIXME: Probably slow.
		if (warmup_finish && ((get_sys_clock() - run_starttime >= DONE_TIMER) || (txn_st_cnt >= MAX_TXN_PER_PART/g_thread_cnt && txn_pool.empty(get_node_id())))) {
        //if (warmup_finish && ((get_sys_clock() - run_starttime >= DONE_TIMER) || (stats.get_txn_cnts() >= MAX_TXN_PER_PART && txn_pool.empty(get_node_id())))) {
			//assert(txn_cnt == MAX_TXN_PER_PART);
			if( !ATOM_CAS(_wl->sim_done, false, true) )
				assert( _wl->sim_done);
			stoptime = get_sys_clock();
		}
    }
		//while(!work_queue.poll_next_query() && !(_wl->sim_done && _wl->sim_timeout)) { }

		// End conditions
		if ((_wl->sim_done && _wl->sim_timeout) || (get_sys_clock() - run_starttime >= DONE_TIMER)) {
#if !NOGRAPHITE
			CarbonDisableModelsBarrier(&enable_barrier);
#endif
			uint64_t currtime = get_sys_clock();
      if(stoptime == 0)
        stoptime = currtime;
			if(currtime - stoptime > MSG_TIMEOUT) {
				SET_STATS(get_thd_id(), tot_run_time, currtime - run_starttime - MSG_TIMEOUT); 
			}
			else {
				SET_STATS(get_thd_id(), tot_run_time, stoptime - run_starttime); 
			}
      if(get_thd_id() == 0) {
        work_queue.finish(currtime);
        abort_queue.abort_finish(currtime);

      // Send EXP_DONE to all nodes
		uint64_t nnodes = g_node_cnt + g_client_node_cnt;
#if CC_ALG == CALVIN
		nnodes++;
#endif
		for(uint64_t i = 0; i < nnodes; i++) {
			if(i != g_node_id) {
				rem_qry_man.send_exp_done(i);
			}
		}
      }
			return FINISH;
		}

		if((m_query = abort_queue.get_next_abort_query(get_thd_id())) != NULL) {
      work_queue.add_query(_thd_id,m_query);
    }

		if((m_query = work_queue.get_next_query(get_thd_id())) == NULL)
			continue;

    uint64_t txn_id = m_query->txn_id;

		rc = RCOK;
		starttime = get_sys_clock();
    debug1 = 0;
    debug2 = m_query->rtype;
    assert(debug2 <= CL_RSP);
    //printf("%ld: %d %ld\n",this->_thd_id,m_query->rtype,m_query->txn_id);

		switch(m_query->rtype) {
			case RPASS:
				m_txn = txn_pool.get_txn(m_query->return_id, m_query->txn_id);
				assert(m_txn != NULL);
        m_txn->register_thd(this);
        rc = m_txn->rc;
				break;
			case RINIT:
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || CC_ALG == VLL
				// This transaction is from a remote node
#if DEBUG_DISTR
				printf("%ld Received RINIT %ld\n",GET_PART_ID_FROM_IDX(_thd_id),m_query->txn_id);
#endif
				assert(CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || m_query->txn_id % g_node_cnt != g_node_id);
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
        assert(m_query->home_part != GET_PART_ID_FROM_IDX(_thd_id));
#endif
				INC_STATS(0,rinit,1);

				// Set up txn_pool
        if((CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC) && m_query->txn_id % g_node_cnt == g_node_id) {
          m_txn = txn_pool.get_txn(m_query->return_id, m_query->txn_id);
          tmp_query = txn_pool.get_qry(m_query->return_id, m_query->txn_id);
          tmp_query->active_part = m_query->active_part;
          //m_query = tmp_query;
        } else {
				  rc = _wl->get_txn_man(m_txn, this);
        }
				assert(rc == RCOK);
				m_txn->set_txn_id(m_query->txn_id);
				m_txn->set_ts(m_query->ts);

#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
        m_txn->home_part = m_query->home_part;
        m_txn->active_part = m_query->active_part;
#endif
				m_txn->set_pid(m_query->pid);
				m_txn->state = INIT;
        if((CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC) && m_query->txn_id % g_node_cnt == g_node_id) {
        } else {
				  txn_pool.add_txn(m_query->return_id,m_txn,m_query);
        }

#if CC_ALG == MVCC
				//glob_manager.add_ts(m_query->return_id, 0, m_query->ts);
				glob_manager.add_ts(m_query->return_id, m_query->thd_id, m_query->ts);
#endif
#if CC_ALG == AVOID
				m_txn->read_keys(m_query);
#endif

        /*
           //TODO: Add me for parallel YCSB!
#if WORKLOAD == YCSB
        // For when we start executing txn
        m_query->rtype = RQRY;
#endif
*/
#if CC_ALG == VLL
        rc = vll_man.beginTxn(m_txn,m_query);
        if(rc == WAIT)
          break;
#endif
				// HStore: lock partitions at this node
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
				part_lock_man.rem_lock(m_query->parts, m_query->part_cnt, m_txn);
#else

        /*
           //TODO: Add me for parallel YCSB!
#if WORKLOAD == YCSB
        // Start executing txn
        txn_pool.restart_txn(m_txn->get_txn_id());
#else
*/

				// Send back ACK
				rem_qry_man.ack_response(m_query);
//#endif

#endif
        m_query = NULL;


				break;
#endif	// CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || CC_ALG == VLL
			case RPREPARE: {
#if DEBUG_DISTR
				printf("%ld Received RPREPARE %ld\n",GET_PART_ID_FROM_IDX(_thd_id),m_query->txn_id);
#endif
				// This transaction is from a remote node
				assert(CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || m_query->txn_id % g_node_cnt != g_node_id);
				INC_STATS(0,rprep,1);
				m_txn = txn_pool.get_txn(m_query->return_id, m_query->txn_id);
				bool validate = true;
#if CC_ALG != HSTORE && CC_ALG != HSTORE_SPEC && CC_ALG != VLL
				if (m_txn == NULL) {
					//assert(m_query->rc == Abort);
					validate = false;
				}
#else
				assert(m_txn != NULL);
#endif
        if(m_txn) {
          m_txn->register_thd(this);
				  tmp_query = txn_pool.get_qry(m_query->return_id, m_query->txn_id);
          m_query = tmp_query->merge(m_query);
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
        assert(m_query->home_part != GET_PART_ID_FROM_IDX(_thd_id));
#endif
        }
				if (validate) {
					m_txn->state = PREP;
					// Validate transaction
					rc  = m_txn->validate();
				}
				// Send ACK w/ commit/abort
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
        if(GET_NODE_ID(m_query->active_part) != GET_NODE_ID(m_query->home_part)) {
          if(validate)
					  m_query->rc = rc;
				  rem_qry_man.ack_response(m_query);
        } else {
         // Model after RACK
      //printf("Local RACK 4 -- %ld\n",_thd_id);
#if WORKLOAD == TPCC
	    base_query * tmp_query = (tpcc_query *) mem_allocator.alloc(sizeof(tpcc_query), 0);
	      tmp_query = new tpcc_query();
#elif WORKLOAD == YCSB
	    base_query * tmp_query = (ycsb_query *) mem_allocator.alloc(sizeof(ycsb_query), 0);
        tmp_query = new ycsb_query();
#endif
        tmp_query->txn_id = m_query->txn_id;
        tmp_query->ts = m_query->ts;
        tmp_query->home_part = m_query->home_part;
        tmp_query->pid = m_query->pid;
        tmp_query->rtype = RACK;
        tmp_query->rc = rc;
        tmp_query->active_part = m_query->home_part;
        work_queue.add_query(GET_PART_ID_IDX(tmp_query->active_part),tmp_query);
        }
#else
          if(validate)
					  m_query->rc = rc;
				  rem_qry_man.ack_response(m_query);
#endif

        m_query = NULL;
				break;
			}
			case RQRY:
				// This transaction is from a remote node
				assert(CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || m_query->txn_id % g_node_cnt != g_node_id);
        assert(CC_ALG != VLL || m_query->rc != Abort);
        /*
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
        assert(m_query->home_part != GET_PART_ID_FROM_IDX(_thd_id));
#endif
*/
				INC_STATS(0,rqry,1);

				// Theoretically, we could send multiple queries to one node.
				//    m_txn could already be in txn_pool

				m_txn = txn_pool.get_txn(m_query->return_id, m_query->txn_id);
#if QRY_ONLY || !(CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || CC_ALG == VLL) 
				if (m_txn == NULL) {
					rc = _wl->get_txn_man(m_txn, this);
					assert(rc == RCOK);
					m_txn->set_txn_id(m_query->txn_id);
					m_txn->set_ts(m_query->ts);
					m_txn->set_pid(m_query->pid);
					m_txn->state = INIT;
					txn_pool.add_txn(m_query->return_id,m_txn,m_query);
				}
#endif
				assert(m_txn != NULL);
        m_txn->register_thd(this);

        // FIXME: May be doing twice the work
				tmp_query = txn_pool.get_qry(m_query->return_id, m_query->txn_id);
        m_query = tmp_query->merge(m_query);
#if DEBUG_DISTR
				if(m_txn->state != EXEC)
					printf("%ld Received RQRY %ld\n",GET_PART_ID_FROM_IDX(_thd_id),m_query->txn_id);
#endif


				m_txn->set_ts(m_query->ts);
#if CC_ALG == OCC
				m_txn->set_start_ts(m_query->start_ts);
#endif

				if(m_txn->state != EXEC) {
					m_txn->state = EXEC;
				} else {
					outstanding_waits--;
					if(outstanding_waits == 0) {
						INC_STATS(get_thd_id(),time_clock_wait,get_sys_clock() - last_waittime);
					}
				}
#if CC_ALG == AVOID
				// RQRY sent because the read we sent during the init phase was outdated
				// Return the updated read value
				m_txn->read_keys_again(m_query);
#endif

				rc = m_txn->run_txn(m_query);

				assert(rc != WAIT_REM);
				timespan = get_sys_clock() - starttime;
				INC_STATS(get_thd_id(),time_rqry,timespan);
        /*
           //TODO: Add me for parallel YCSB!
#if WORKLOAD == YCSB
				if(rc != WAIT)
				  rem_qry_man.ack_response(m_query);
#else
*/
				if(rc != WAIT)
					rem_qry_man.remote_rsp(m_query,m_txn);
#if QRY_ONLY
				txn_pool.delete_txn(m_query->return_id, m_query->txn_id);
#endif
//#endif
        m_query = NULL;
				break;
			case RQRY_RSP:
#if DEBUG_DISTR
				printf("%ld Received RQRY_RSP %ld\n",GET_PART_ID_FROM_IDX(_thd_id),m_query->txn_id);
#endif
				// This transaction originated from this node
				assert(m_query->txn_id % g_node_cnt == g_node_id);
				INC_STATS(0,rqry_rsp,1);
				m_txn = txn_pool.get_txn(g_node_id, m_query->txn_id);
				assert(m_txn != NULL);
        m_txn->register_thd(this);
        if(m_txn->txn_stat_starttime > 0) {
          m_txn->txn_time_net += get_sys_clock() - m_txn->txn_stat_starttime;
          m_txn->txn_stat_starttime = 0;
        }
				tmp_query = txn_pool.get_qry(m_query->return_id, m_query->txn_id);
        m_query = tmp_query->merge(m_query);
				INC_STATS(get_thd_id(),time_wait_rem,get_sys_clock() - m_txn->wait_starttime);

				outstanding_rwaits--;
				if(outstanding_rwaits == 0) {
					INC_STATS(get_thd_id(),time_clock_rwait,get_sys_clock() - last_rwaittime);
				}

				// Execute from original txn; Note: txn may finish
				assert(m_txn->get_rsp_cnt() == 0);
				rc = m_txn->run_txn(m_query);
#if QRY_ONLY
        m_query->client_id =txn_pool.get_qry(g_node_id, m_query->txn_id)->client_id; 
        //m_query =txn_pool.get_qry(g_node_id, m_query->txn_id); 
#endif
				break;
			case RFIN: {
#if DEBUG_DISTR
				printf("%ld Received RFIN %ld\n",GET_PART_ID_FROM_IDX(_thd_id),m_query->txn_id);
#endif
				// This transaction is from a remote node
				assert(CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || m_query->txn_id % g_node_cnt != g_node_id);
				INC_STATS(0,rfin,1);
				m_txn = txn_pool.get_txn(m_query->return_id, m_query->txn_id);
				bool finish = true;
#if CC_ALG != HSTORE && CC_ALG != HSTORE_SPEC && CC_ALG != VLL
				if (m_txn == NULL) {
					assert(m_query->rc == Abort);
					finish = false;	
				}
#else
				// Transaction should ALWAYS be in the pool until node receives RFIN
				assert(m_txn != NULL);
#endif
        if(m_txn) {
          m_txn->register_thd(this);
#if CC_ALG != HSTORE && CC_ALG != HSTORE_SPEC
				  tmp_query = txn_pool.get_qry(m_query->return_id, m_query->txn_id);
          m_query = tmp_query->merge(m_query);
#else
          if(GET_NODE_ID(m_query->home_part) != g_node_id || GET_PART_ID_IDX(m_query->home_part) == _thd_id) {
				    tmp_query = txn_pool.get_qry(m_query->return_id, m_query->txn_id);
            m_query = tmp_query->merge(m_query);
        assert(m_query->home_part != GET_PART_ID_FROM_IDX(_thd_id));
          } else {
          }
#endif
        }
				if (finish) {
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
          if(GET_NODE_ID(m_query->home_part) != g_node_id || GET_PART_ID_IDX(m_query->home_part) == _thd_id) {
					  m_txn->state = DONE;
					  m_txn->rem_fin_txn(m_query);
					  txn_pool.delete_txn(m_query->return_id, m_query->txn_id);
          } else {
					  m_txn->loc_fin_txn(m_query);
          }
#else
					m_txn->state = DONE;
					m_txn->rem_fin_txn(m_query);
					txn_pool.delete_txn(m_query->return_id, m_query->txn_id);
#endif
				}

#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
        if(GET_NODE_ID(m_query->home_part) != g_node_id) {
				  rem_qry_man.ack_response(m_query);
        } else {
         // Model after RACK
      //printf("Local RACK 5 -- %ld\n",_thd_id);
#if WORKLOAD == TPCC
	    base_query * tmp_query = (tpcc_query *) mem_allocator.alloc(sizeof(tpcc_query), 0);
	      tmp_query = new tpcc_query();
#elif WORKLOAD == YCSB
	    base_query * tmp_query = (ycsb_query *) mem_allocator.alloc(sizeof(ycsb_query), 0);
        tmp_query = new ycsb_query();
#endif
        tmp_query->txn_id=m_query->txn_id;
        tmp_query->ts = m_query->ts;
        tmp_query->home_part = m_query->home_part;
        tmp_query->pid = m_query->pid;
        tmp_query->rtype = RACK;
        tmp_query->rc = rc;
        tmp_query->active_part = m_query->home_part;
        work_queue.add_query(GET_PART_ID_IDX(tmp_query->active_part),tmp_query);
        }
#else
				  rem_qry_man.ack_response(m_query);
#endif


				m_query = NULL;
				break;
			}
			case RACK:
#if DEBUG_DISTR
				printf("%ld Received RACK %ld\n",GET_PART_ID_FROM_IDX(_thd_id),m_query->txn_id);
#endif
				// This transaction originated from this node
				assert(m_query->txn_id % g_node_cnt == g_node_id);
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
				assert(GET_PART_ID_IDX(m_query->home_part) == _thd_id);
#endif
				INC_STATS(0,rack,1);
				m_txn = txn_pool.get_txn(g_node_id, m_query->txn_id);
				assert(m_txn != NULL);
        m_txn->register_thd(this);
				tmp_query = txn_pool.get_qry(m_query->return_id, m_query->txn_id);
        m_query = tmp_query->merge(m_query);

        // returns the current response count for this transaction
				rsp_cnt = m_txn->decr_rsp(1);
        assert(rsp_cnt >=0);

				if(rsp_cnt == 0) {
					// Done waiting 
					INC_STATS(get_thd_id(),time_wait_rem,get_sys_clock() - m_txn->wait_starttime);
          //FIXME: this was uncommented. should we always say ok?
					m_txn->rc = RCOK;

					// After RINIT
					switch(m_txn->state) {
						case INIT:
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
              
#if CC_ALG == HSTORE_SPEC
							txn_pool.commit_spec_ex(RCOK,_thd_id);
							spec_man.clear();
#endif
							uint64_t part_arr_s[1];
							part_arr_s[0] = m_query->part_to_access[0];
							rc = part_lock_man.rem_lock(part_arr_s, 1, m_txn);
							m_query->update_rc(rc);
							if(rc == WAIT) {
								m_txn->rc = rc;
					      m_query = NULL;
								break;
							}

							if(m_txn->ready_part > 0) {
					      m_query = NULL;
								break;
              }
#endif
#if CC_ALG == VLL
              // This may return an Abort

        if(m_query->rc == Abort) {
							m_txn->state = FIN;
							// send rfin messages w/ abort
							m_txn->finish(m_query,true);
					    m_query = NULL;
              break;
        }
        rc = vll_man.beginTxn(m_txn,m_query);
        if(rc == WAIT)
          break;
        if(rc == Abort) {
							m_txn->state = FIN;
							// send rfin messages w/ abort
							m_txn->finish(m_query,true);
					    m_query = NULL;
              break;
        }
#endif
							m_txn->state = EXEC; 
							txn_pool.restart_txn(m_txn->get_txn_id());
					    m_query = NULL;
							break;
						case PREP:
							// Validate
              debug1 = 1;
							m_txn->spec = true;
							rc  = m_txn->validate();
							m_txn->spec = false;
							if(rc == Abort)
								m_query->rc = rc;
#if CC_ALG == HSTORE_SPEC
							if(m_query->rc != Abort) {
								// allow speculative execution to start
								txn_pool.start_spec_ex(_thd_id);
							}
#endif

							m_txn->state = FIN;
							// After RPREPARE, send rfin messages
							m_txn->finish(m_query,true);
              // Note: can't touch m_txn after this, since all other
              //  RACKs may have been received and FIN processed before this point
					    m_query = NULL;
							break;
						case FIN:
							// After RFIN
              debug1 = 2;
							m_txn->state = DONE;
#if CC_ALG == HSTORE_SPEC
							txn_pool.commit_spec_ex(m_query->rc,_thd_id);
							spec_man.clear();
#endif
							uint64_t part_arr[1];
							part_arr[0] = m_query->part_to_access[0];
							rc = m_txn->finish(m_query->rc,part_arr,1);
							break;
						default:
							assert(false);
					}
				} else {
					m_query = NULL;
        }
        // Possible race condition here if processing 2 RACKs and 1 thread updates txn state to DONE
/*
				if(m_txn->state != DONE)
					m_query = NULL;
*/
				break;
			case RTXN:
				// This transaction is originating at this node
				assert(m_query->txn_id == UINT64_MAX || (m_query->txn_id % g_node_cnt == g_node_id));

				INC_STATS(0,rtxn,1);

				if(m_query->txn_id == UINT64_MAX) {
					ATOM_ADD(txn_st_cnt,1);
					// New transaction
					rc = _wl->get_txn_man(m_txn, this);
					assert(rc == RCOK);

					m_txn->abort_cnt = 0;
					if (CC_ALG == WAIT_DIE || CC_ALG == VLL) {
						m_txn->set_ts(get_next_ts());
						m_query->ts = m_txn->get_ts();
					}
					if (CC_ALG == MVCC) {
						m_query->thd_id = _thd_id;
					}

#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
          m_txn->home_part = m_query->home_part;
          m_txn->active_part = m_query->active_part;
#endif
					m_txn->set_pid(m_query->pid);

					// Only set new txn_id when txn first starts
					m_txn->set_txn_id( ( get_node_id() + get_thd_id() * g_node_cnt) 
							+ (g_thread_cnt * g_node_cnt * thd_txn_id));
					//m_txn->set_txn_id( (get_thd_id() + get_node_id() * g_thread_cnt) + (g_thread_cnt * g_node_cnt * thd_txn_id));
					thd_txn_id ++;
					m_query->set_txn_id(m_txn->get_txn_id());
          work_queue.update_hash(get_thd_id(),m_txn->get_txn_id());
          txn_id= m_txn->get_txn_id();

          /*
					if(m_query->part_num > 1) {
						INC_STATS(get_thd_id(),mpq_cnt,1);
					}
          */
					// Put txn in txn_pool in case of WAIT
					txn_pool.add_txn(g_node_id,m_txn,m_query);

          //FIXME: move clearing query variables elsewhere?
          m_query->penalty = 0;

					m_txn->starttime = get_sys_clock();
          m_txn->txn_time_misc += m_query->time_q_work;
          m_txn->txn_time_misc += m_query->time_copy;
          m_txn->register_thd(this);
#if DEBUG_DISTR && !DEBUG_TIMELINE
					printf("START %ld %f\n",m_txn->get_txn_id(),(double)(m_txn->starttime - run_starttime) / BILLION);
#endif
#if DEBUG_TIMELINE
					printf("START %ld %ld\n",m_txn->get_txn_id(),m_txn->starttime);
					//printf("START %ld %ld\n",m_txn->get_txn_id(),m_txn->starttime - run_starttime);
#endif
				}
				else {
					// Re-executing transaction
					m_txn = txn_pool.get_txn(g_node_id,m_query->txn_id);
					// 
					assert(m_txn != NULL);
          m_txn->register_thd(this);
				}

				if(m_txn->rc != WAIT && m_txn->state == START) {

					if ((CC_ALG == HSTORE && !HSTORE_LOCAL_TS)
							|| (CC_ALG == HSTORE_SPEC && !HSTORE_LOCAL_TS)
							|| CC_ALG == MVCC 
              || CC_ALG == VLL
							|| CC_ALG == TIMESTAMP) { 
						m_txn->set_ts(get_next_ts());
						m_query->ts = m_txn->get_ts();
#if DEBUG_DISTR
					//printf("GET_TIMESTAMP %ld %lu\n",m_txn->get_txn_id(),m_txn->get_ts());
#endif
					}

#if CC_ALG == MVCC
					glob_manager.add_ts(get_thd_id(), m_txn->get_ts());
#elif CC_ALG == OCC
					m_txn->start_ts = get_next_ts(); 
					m_query->start_ts = m_txn->get_start_ts();
#endif

					ttime = get_sys_clock();
#if CC_ALG != HSTORE && CC_ALG != HSTORE_SPEC && CC_ALG != VLL
          // Touch first partition
          m_query->part_touched[m_query->part_touched_cnt++] = m_query->part_to_access[0];
#endif
#if !QRY_ONLY && (CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC || CC_ALG == VLL)
					m_txn->state = INIT;
					for(uint64_t i = 0; i < m_query->part_num; i++) {
						uint64_t part_id = m_query->part_to_access[i];
						// Check for duplicate partitions so we don't send init twice
						uint64_t j;
						bool sent = false;
						for(j = 0; j < i; j++) {
							if(part_id == m_query->part_to_access[j]) {
								sent = true;
							}
						}
						if(sent)
							continue;
						//if(GET_NODE_ID(part_id) != g_node_id) {
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
						if(part_id != m_query->home_part) {
							m_txn->incr_rsp(1);
						}
#else
						if(GET_NODE_ID(part_id) != g_node_id) {
							m_txn->incr_rsp(1);
						}
#endif
					}

#if CC_ALG == HSTORE_SPEC
          if(m_query->part_num > 1) {
						// allow speculative execution to start // FIXME: Only allow for this specific partition
						txn_pool.start_spec_ex(_thd_id);
          }
#endif

          // FIXME: Only do if not in specx 
          if(CC_ALG != HSTORE_SPEC || !m_txn->spec) {

					// Send RINIT message to all involved nodes
					for(uint64_t i = 0; i < m_query->part_num; i++) {
						uint64_t part_id = m_query->part_to_access[i];
						// Check for duplicate partitions so we don't send init twice
						uint64_t j;
						bool sent = false;
						for(j = 0; j < i; j++) {
							if(part_id == m_query->part_to_access[j]) {
								sent = true;
							}
						}
						if(sent)
							continue;

#if CC_ALG == VLL
              if(GET_NODE_ID(part_id) != g_node_id) {
                //m_txn->incr_rsp(1);
                rc = WAIT;
                m_txn->rc = rc;
                m_query->dest_part = part_id;
                rem_qry_man.send_init(m_query,part_id);
                m_txn->wait_starttime = get_sys_clock();
                m_query->part_touched[m_query->part_touched_cnt++] = part_id;
              } else {
							// local init: MPQ Moved to after RINITs return
                if(m_query->part_num == 1) {
                  rc = vll_man.beginTxn(m_txn,m_query);
                  if(rc == WAIT) {
                    break;
                    //m_txn->rc = rc;
                  }
                }
              }
#endif

#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
            assert(m_query->active_part == m_query->home_part);
						if(part_id != m_query->home_part) {
              if(GET_NODE_ID(part_id) != g_node_id) {
                //m_txn->incr_rsp(1);
                rc = WAIT;
                m_txn->rc = rc;
                m_query->dest_part = part_id;
                rem_qry_man.send_init(m_query,part_id);
                m_txn->wait_starttime = get_sys_clock();
                m_query->part_touched[m_query->part_touched_cnt++] = part_id;
              } else {
                assert(CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC);
                rc = WAIT;
                m_txn->rc = rc;
                m_query->part_touched[m_query->part_touched_cnt++] = part_id;
                // initialize new query and place on work queue
#if WORKLOAD == TPCC
	    base_query * tmp_query = (tpcc_query *) mem_allocator.alloc(sizeof(tpcc_query), 0);
	      tmp_query = new tpcc_query();
#elif WORKLOAD == YCSB
	    base_query * tmp_query = (ycsb_query *) mem_allocator.alloc(sizeof(ycsb_query), 0);
        tmp_query = new ycsb_query();
#endif
        tmp_query->txn_id = m_query->txn_id;
        tmp_query->ts = m_query->ts;
        tmp_query->home_part = m_query->home_part;
        tmp_query->rtype = RINIT;
        tmp_query->active_part = part_id;
        tmp_query->part_cnt = 1;
        tmp_query->parts = new uint64_t[1];
        tmp_query->parts[0] = part_id;
        work_queue.add_query(GET_PART_ID_IDX(tmp_query->active_part),tmp_query);
                

              }
						} else {
							// local init: MPQ Moved to after RINITs return
              if(m_query->part_num == 1) {
							uint64_t part_arr[1];
							part_arr[0] = part_id;
							rc2 = part_lock_man.rem_lock(part_arr, 1, m_txn);
							m_query->update_rc(rc2);
							if(rc2 == WAIT) {
								rc = WAIT;
								m_txn->rc = rc;
							}
              }
						}
#endif
					} // for(uint64_t i = 0; i < m_query->part_num; i++) 
          }

#endif // !QRY_ONLY
					INC_STATS(get_thd_id(),time_msg_sent,get_sys_clock() - ttime);
				} //if(m_txn->rc != WAIT) 

				if(m_query->rc == Abort && rc == WAIT) {
					m_query = NULL;
					break;
				}

				if(rc == RCOK) {
					assert(m_txn->get_rsp_cnt() == 0);
					if(m_txn->state != EXEC) {
						m_txn->state = EXEC;
					} else {
						outstanding_waits--;
						if(outstanding_waits == 0) {
							INC_STATS(get_thd_id(),time_clock_wait,get_sys_clock() 
									- last_waittime);
						}
					}
					rc = m_txn->run_txn(m_query);
				}

				break;
			default:
				assert(false);
				break;
		}

		// If m_query was freed just before this, m_query is NULL
		//if(m_query != NULL && GET_NODE_ID(m_query->pid) == g_node_id) {
		if(m_query != NULL && m_query->txn_id % g_node_cnt == g_node_id) {
			switch(rc) {
				case RCOK:
#if CC_ALG == HSTORE_SPEC
					if(m_txn->spec && m_txn->state != DONE)
						break;
					// Transaction is finished. Increment stats. Remove from txn pool
          if(m_txn->spec) {
            assert(m_query->part_num == 1);
            INC_STATS(0,spec_commit_cnt,1);
          }
#endif
					assert(m_txn->get_rsp_cnt() == 0);
					if(m_query->part_num == 1 && !m_txn->spec) {
						rc = m_txn->finish(rc,m_query->part_to_access,m_query->part_num);
					} else
						assert(m_txn->state == DONE || QRY_ONLY || TWOPC_ONLY);
					INC_STATS(get_thd_id(),txn_cnt,1);
					if(m_txn->abort_cnt > 0) { 
						INC_STATS(get_thd_id(), txn_abort_cnt, 1);
						INC_STATS_ARR(get_thd_id(), all_abort, m_txn->abort_cnt);
					}
					if(m_query->part_num > 1) {
						INC_STATS(get_thd_id(),mpq_cnt,1);
					}
					timespan = get_sys_clock() - m_txn->starttime;
					INC_STATS(get_thd_id(), run_time, timespan);
					INC_STATS(get_thd_id(), latency, timespan);
          INC_STATS_ARR(get_thd_id(),all_lat,timespan);

          m_txn->txn_time_q_abrt += m_query->time_q_abrt;
          m_txn->txn_time_q_work += m_query->time_q_work;
          m_txn->txn_time_copy += m_query->time_copy;
          m_txn->txn_time_misc += timespan;
          m_txn->update_stats();

#if DEBUG_DISTR && !DEBUG_TIMELINE
					printf("COMMIT %ld %f\n",m_txn->get_txn_id(),(double)(get_sys_clock() - run_starttime) / BILLION);
#endif
#if DEBUG_TIMELINE
					printf("COMMIT %ld %ld\n",m_txn->get_txn_id(),get_sys_clock());
					//printf("COMMIT %ld %ld\n",m_txn->get_txn_id(),get_sys_clock() - run_starttime);
#endif
					// Send "result" back to client
          assert(m_query->part_num == 1 || debug1 == 2);
          assert((m_query->part_num == 1 && (m_query->rtype == RTXN || m_query->rtype == RPASS)) || (m_query->part_num > 1 && m_query->rtype == RACK));
					rem_qry_man.send_client_rsp(m_query);
					txn_pool.delete_txn(g_node_id,m_query->txn_id);
					txn_cnt++;
					//ATOM_SUB(txn_pool.inflight_cnt,1);
          /*
#if CC_ALG == VLL
          vll_man.restartQFront();
#endif
*/
					break;
				case Abort:
					assert(m_txn->get_rsp_cnt() == 0);
					if(m_query->part_num == 1 && !m_txn->spec)
						rc = m_txn->finish(rc,m_query->part_to_access,m_query->part_num);
          /*
					else
						assert(m_txn->state == DONE);
            */
					assert(m_txn != NULL);
					assert(m_query->txn_id != UINT64_MAX);

#if WORKLOAD == TPCC
        if(((tpcc_query*)m_query)->rbk && m_query->rem_req_state == TPCC_FIN) {
          INC_STATS(get_thd_id(),txn_cnt,1);
			    INC_STATS(get_thd_id(), rbk_abort_cnt, 1);
		      timespan = get_sys_clock() - m_txn->starttime;
		      INC_STATS(get_thd_id(), run_time, timespan);
		      INC_STATS(get_thd_id(), latency, timespan);
        txn_pool.delete_txn(g_node_id,m_query->txn_id);
        txn_cnt++;
        //ATOM_SUB(txn_pool.inflight_cnt,1);
					rem_qry_man.send_client_rsp(m_query);
          break;
        
        }
#endif
        //rc = m_txn->finish(rc);
        //txn_pool.add_txn(g_node_id,m_txn,m_query);
        // Stats
				INC_STATS(get_thd_id(), abort_cnt, 1);

				//printf("ABORT %ld %ld %ld\n",m_txn->get_txn_id(),m_query->part_num,m_query->part_touched_cnt);

        m_txn->abort_cnt++;
        m_query->part_touched_cnt = 0;
        m_query->rtype = RTXN;
        m_query->rc = RCOK;
        m_query->spec = false;
        m_query->spec_done = false;
        m_query->reset();
#if CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC
        m_query->active_part = m_query->home_part;
#endif
        m_txn->state = START;
        m_txn->rc = RCOK;
        m_txn->cc_wait_abrt_cnt += m_txn->cc_wait_cnt;
        m_txn->cc_wait_abrt_time += m_txn->cc_wait_time;
        m_txn->cc_hold_abrt_time += m_txn->cc_hold_time;
        m_txn->clear();

#if DEBUG_TIMELINE
					printf("ABORT %ld %ld\n",m_txn->get_txn_id(),get_sys_clock());
					//printf("ABORT %ld %ld\n",m_txn->get_txn_id(),get_sys_clock() - run_starttime);
#endif
					m_txn->penalty_start = get_sys_clock();

					//m_query->penalty_start = m_txn->penalty_start;

#if CC_ALG == HSTORE_SPEC
          if(m_txn->spec) {
            INC_STATS(0,spec_abort_cnt,1);
            m_txn->spec = false;
            m_txn->spec_done = false;
					  work_queue.add_query(_thd_id,m_query);
            break;
          }
#endif
          m_txn->spec = false;
          m_txn->spec_done = false;
          abort_queue.add_abort_query(_thd_id,m_query);
					//work_queue.add_query(m_query);
          /*
#if CC_ALG == VLL
          vll_man.restartQFront();
#endif
*/
					break;
				case WAIT:
					assert(m_txn != NULL);
					if(m_txn->state != INIT) { 
						last_waittime = get_sys_clock();
						outstanding_waits++;
					}
					//m_txn->wait_starttime = get_sys_clock();
					break;
				case WAIT_REM:
					// Save transaction information for later
					assert(m_txn != NULL);
					m_txn->wait_starttime = get_sys_clock();
					last_rwaittime = get_sys_clock();
					outstanding_rwaits++;
					//txn_pool.add_txn(g_node_id,m_txn,m_query);
#if WORKLOAD == TPCC
					// WAIT_REM from finish
					if(m_query->rem_req_state == TPCC_FIN)
						break;
#elif WORKLOAD == YCSB
					if(m_query->rem_req_state == YCSB_FIN)
						break;
#endif
					assert((CC_ALG == HSTORE || CC_ALG == HSTORE_SPEC) || m_query->dest_id != g_node_id);
#if CC_ALG != HSTORE && CC_ALG != HSTORE_SPEC
          m_query->part_touched[m_query->part_touched_cnt++] = GET_PART_ID(0,m_query->dest_id);
#endif
          // Stat start for txn_time_net
          m_txn->txn_stat_starttime = get_sys_clock();

					rem_qry_man.remote_qry(m_query,m_query->rem_req_state,m_query->dest_id,m_txn);
					break;
				default:
					assert(false);
					break;
			}
		} 

    work_queue.done(get_thd_id(),txn_id);

		timespan = get_sys_clock() - starttime;
		INC_STATS(get_thd_id(),time_work,timespan);

		if (!warmup_finish && txn_st_cnt >= WARMUP / g_thread_cnt) 
		{
			stats.clear( get_thd_id() );
#if !NOGRAPHITE
			CarbonDisableModelsBarrier(&enable_barrier);
#endif
			return FINISH;
		}

		if (warmup_finish && ((get_sys_clock() - run_starttime >= DONE_TIMER) || (txn_st_cnt >= MAX_TXN_PER_PART/g_thread_cnt && txn_pool.empty(get_node_id())))) {
			//assert(txn_cnt == MAX_TXN_PER_PART);
			if( !ATOM_CAS(_wl->sim_done, false, true) )
				assert( _wl->sim_done);
			stoptime = get_sys_clock();
		}


	}

}


ts_t
thread_t::get_next_ts() {
	if (g_ts_batch_alloc) {
		if (_curr_ts % g_ts_batch_num == 0) {
			_curr_ts = glob_manager.get_ts(get_thd_id());
			_curr_ts ++;
		} else {
			_curr_ts ++;
		}
		return _curr_ts - 1;
	} else {
		_curr_ts = glob_manager.get_ts(get_thd_id());
		return _curr_ts;
	}
}

