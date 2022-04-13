
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <linux/limits.h>
#elif defined(__FreeBSD__)
#include <sys/stat.h>
#elif defined(__APPLE__)
#include <sys/syslimits.h>
#else
#include <limits.h>
#endif

#include <boost/asio.hpp>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <fdb.hpp>
#include "fdbclient/zipf.h"
#include "logger.hpp"
#include "mako.hpp"
#include "process.hpp"
#include "utils.hpp"
#include "future.hpp"
#include "async.hpp"

using namespace fdb;
using namespace mako;

thread_local Logger logr = Logger(MainProcess{}, VERBOSE_DEFAULT);

/* cleanup database */
int cleanup(Transaction tx, Arguments const& args) {
	auto beginstr = ByteString{};
	beginstr.reserve(args.key_length);
	genKeyPrefix(beginstr, KEY_PREFIX, args);
	beginstr.push_back(0);

	auto endstr = ByteString{};
	endstr.reserve(args.key_length);
	genKeyPrefix(endstr, KEY_PREFIX, args);
	endstr.push_back(0xff);

	auto watch = Stopwatch(StartAtCtor{});

	while (true) {
		tx.clearRange(beginstr, endstr);
		auto future_commit = tx.commit();
		const auto rc = waitAndHandleError(tx, future_commit, "COMMIT_CLEANUP");
		if (rc == FutureRC::OK) {
			break;
		} else if (rc == FutureRC::RETRY || rc == FutureRC::CONFLICT) {
			// tx already reset
			continue;
		} else {
			return -1;
		}
	}

	tx.reset();
	logr.info("Clear range: {:6.3f} sec", toDoubleSeconds(watch.stop().diff()));
	return 0;
}

/* populate database */
int populate(Transaction tx,
             Arguments const& args,
             int worker_id,
             int thread_id,
             int thread_tps,
             ThreadStatistics& stats,
             LatencySampleBinArray& sample_bins) {
	const auto key_begin = insertBegin(args.rows, worker_id, thread_id, args.num_processes, args.num_threads);
	const auto key_end = insertEnd(args.rows, worker_id, thread_id, args.num_processes, args.num_threads);
	auto xacts = 0;

	auto keystr = ByteString{};
	auto valstr = ByteString{};
	keystr.reserve(args.key_length);
	valstr.reserve(args.value_length);
	const auto num_commit_every = args.txnspec.ops[OP_INSERT][OP_COUNT];
	const auto num_seconds_trace_every = args.txntrace;
	auto watch_total = Stopwatch(StartAtCtor{});
	auto watch_throttle = Stopwatch(watch_total.getStart());
	auto watch_tx = Stopwatch(watch_total.getStart());
	auto watch_trace = Stopwatch(watch_total.getStart());
	auto key_checkpoint = key_begin; // in case of commit failure, restart from this key

	for (auto i = key_begin; i <= key_end; i++) {
		/* sequential keys */
		genKey(keystr, KEY_PREFIX, args, i);
		/* random values */
		randomString(valstr, args.value_length);

		while (thread_tps > 0 && xacts >= thread_tps /* throttle */) {
			if (toIntegerSeconds(watch_throttle.stop().diff()) >= 1) {
				xacts = 0;
				watch_throttle.startFromStop();
			} else {
				usleep(1000);
			}
		}
		if (num_seconds_trace_every) {
			if (toIntegerSeconds(watch_trace.stop().diff()) >= num_seconds_trace_every) {
				watch_trace.startFromStop();
				logr.debug("txn tracing {}", toCharsRef(keystr));
				auto err = Error{};
				err = tx.setOptionNothrow(FDB_TR_OPTION_DEBUG_TRANSACTION_IDENTIFIER, keystr);
				if (err) {
					logr.error("setOption(TR_OPTION_DEBUG_TRANSACTION_IDENTIFIER): {}", err.what());
				}
				err = tx.setOptionNothrow(FDB_TR_OPTION_LOG_TRANSACTION, BytesRef());
				if (err) {
					logr.error("setOption(TR_OPTION_LOG_TRANSACTION): {}", err.what());
				}
			}
		}

		/* insert (SET) */
		tx.set(keystr, valstr);
		stats.incrOpCount(OP_INSERT);

		/* commit every 100 inserts (default) or if this is the last key */
		if (i == key_end || (i - key_begin + 1) % num_commit_every == 0) {
			const auto do_sample = (stats.getOpCount(OP_TASK) % args.sampling) == 0;
			auto watch_commit = Stopwatch(StartAtCtor{});
			auto future_commit = tx.commit();
			const auto rc = waitAndHandleError(tx, future_commit, "COMMIT_POPULATE_INSERT");
			watch_commit.stop();
			watch_tx.setStop(watch_commit.getStop());
			auto tx_restarter = ExitGuard([&watch_tx]() { watch_tx.startFromStop(); });
			if (rc == FutureRC::OK) {
				key_checkpoint = i + 1; // restart on failures from next key
				tx.reset();
			} else if (rc == FutureRC::ABORT) {
				return -1;
			} else {
				i = key_checkpoint - 1; // restart from last committed
				continue;
			}
			/* xact latency stats */
			if (do_sample) {
				const auto commit_latency = watch_commit.diff();
				const auto tx_duration = watch_tx.diff();
				stats.addLatency(OP_COMMIT, commit_latency);
				stats.addLatency(OP_TRANSACTION, tx_duration);
				sample_bins[OP_COMMIT].put(commit_latency);
				sample_bins[OP_TRANSACTION].put(tx_duration);
			}
			stats.incrOpCount(OP_COMMIT);
			stats.incrOpCount(OP_TRANSACTION);

			stats.incrOpCount(OP_TASK);
			xacts++; /* for throttling */
		}
	}
	logr.debug("Populated {} rows [{}, {}]: {:6.3f} sec",
	           key_end - key_begin + 1,
	           key_begin,
	           key_end,
	           toDoubleSeconds(watch_total.stop().diff()));

	return 0;
}

/* run one transaction */
int runOneTask(Transaction tx, Arguments const& args, ThreadStatistics& stats, LatencySampleBinArray& sample_bins) {
	// reuse memory for keys to avoid realloc overhead
	auto key1 = ByteString{};
	key1.reserve(args.key_length);
	auto key2 = ByteString{};
	key2.reserve(args.key_length);
	auto val = ByteString{};
	val.reserve(args.value_length);

	auto watch_tx = Stopwatch(StartAtCtor{});
	auto watch_task = Stopwatch(watch_tx.getStart());

	auto op_iter = getOpBegin(args);
	auto needs_commit = false;
	auto watch_per_op = std::array<Stopwatch, MAX_OP>{};
	const auto do_sample = (stats.getOpCount(OP_TASK) % args.sampling) == 0;
	while (op_iter != OpEnd) {
		const auto [op, count, step] = op_iter;
		const auto step_kind = opTable[op].stepKind(step);
		auto watch_step = Stopwatch{};
		watch_step.start();
		if (step == 0 /* first step */)
			watch_per_op[op] = Stopwatch(watch_step.getStart());
		auto f = opTable[op].stepFunction(step)(tx, args, key1, key2, val);
		auto future_rc = FutureRC::OK;
		if (f) {
			if (step_kind != StepKind::ON_ERROR) {
				future_rc = waitAndHandleError(tx, f, opTable[op].name());
			} else {
				future_rc = waitAndHandleForOnError(tx, f, opTable[op].name());
			}
		}
		watch_step.stop();
		if (future_rc != FutureRC::OK) {
			if (future_rc == FutureRC::CONFLICT) {
				stats.incrConflictCount();
			} else if (future_rc == FutureRC::RETRY) {
				stats.incrErrorCount(op);
			} else {
				// abort
				tx.reset();
				return -1;
			}
			// retry from first op
			op_iter = getOpBegin(args);
			needs_commit = false;
			continue;
		}
		// step successful
		if (step_kind == StepKind::COMMIT) {
			// reset transaction boundary
			const auto step_latency = watch_step.diff();
			watch_tx.setStop(watch_step.getStop());
			if (do_sample) {
				const auto tx_duration = watch_tx.diff();
				stats.addLatency(OP_COMMIT, step_latency);
				stats.addLatency(OP_TRANSACTION, tx_duration);
				sample_bins[OP_COMMIT].put(step_latency);
				sample_bins[OP_TRANSACTION].put(tx_duration);
			}
			tx.reset();
			watch_tx.startFromStop(); // new tx begins
			stats.incrOpCount(OP_COMMIT);
			stats.incrOpCount(OP_TRANSACTION);
			needs_commit = false;
		}

		// op completed successfully
		if (step + 1 == opTable[op].steps() /* last step */) {
			if (opTable[op].needsCommit())
				needs_commit = true;
			watch_per_op[op].setStop(watch_step.getStop());
			if (do_sample) {
				const auto op_latency = watch_per_op[op].diff();
				stats.addLatency(op, op_latency);
				sample_bins[op].put(op_latency);
			}
			stats.incrOpCount(op);
		}
		// move to next op
		op_iter = getOpNext(args, op_iter);

		// reached the end?
		if (op_iter == OpEnd && (needs_commit || args.commit_get)) {
			auto watch_commit = Stopwatch(StartAtCtor{});
			auto f = tx.commit();
			const auto rc = waitAndHandleError(tx, f, "COMMIT_AT_TX_END");
			watch_commit.stop();
			watch_tx.setStop(watch_commit.getStop());
			auto tx_resetter = ExitGuard([&watch_tx, &tx]() {
				tx.reset();
				watch_tx.startFromStop();
			});
			if (rc == FutureRC::OK) {
				if (do_sample) {
					const auto commit_latency = watch_commit.diff();
					const auto tx_duration = watch_tx.diff();
					stats.addLatency(OP_COMMIT, commit_latency);
					stats.addLatency(OP_TRANSACTION, tx_duration);
					sample_bins[OP_COMMIT].put(commit_latency);
					sample_bins[OP_TRANSACTION].put(tx_duration);
				}
				stats.incrOpCount(OP_COMMIT);
				stats.incrOpCount(OP_TRANSACTION);
			} else {
				if (rc == FutureRC::CONFLICT)
					stats.incrConflictCount();
				else
					stats.incrErrorCount(OP_COMMIT);
				if (rc == FutureRC::ABORT) {
					return -1;
				}
				// restart from beginning
				op_iter = getOpBegin(args);
			}
			needs_commit = false;
		}
	}
	// one task iteration has completed successfully
	const auto task_duration = watch_task.stop().diff();
	if (stats.getOpCount(OP_TASK) % args.sampling == 0) {
		sample_bins[OP_TASK].put(task_duration);
		stats.addLatency(OP_TASK, task_duration);
	}
	stats.incrOpCount(OP_TASK);
	/* make sure to reset transaction */
	tx.reset();
	return 0;
}

int runWorkload(Transaction tx,
                Arguments const& args,
                int const thread_tps,
                std::atomic<double> const& throttle_factor,
                int const thread_iters,
                std::atomic<int> const& signal,
                ThreadStatistics& stats,
                LatencySampleBinArray& sample_bins,
                int const dotrace,
                int const dotagging) {
	auto traceid = std::string{};
	auto tagstr = std::string{};

	if (thread_tps < 0)
		return 0;

	if (dotrace)
		traceid.reserve(32);

	if (dotagging)
		tagstr.reserve(16);

	auto current_tps = static_cast<int>(thread_tps * throttle_factor.load());

	auto time_prev = steady_clock::now();
	auto time_last_trace = time_prev;

	auto rc = 0;
	auto xacts = 0;
	auto total_xacts = int64_t{};
	/* main transaction loop */
	while (1) {
		while ((thread_tps > 0) && (xacts >= current_tps)) {
			/* throttle on */
			const auto time_now = steady_clock::now();
			if (toDoubleSeconds(time_now - time_prev) >= 1.0) {
				/* more than 1 second passed, no need to throttle */
				xacts = 0;
				time_prev = time_now;

				/* update throttle rate */
				current_tps = static_cast<int>(thread_tps * throttle_factor.load());
			} else {
				usleep(1000);
			}
		}
		/* enable transaction trace */
		if (dotrace) {
			const auto time_now = steady_clock::now();
			if (toIntegerSeconds(time_now - time_last_trace) >= 1) {
				time_last_trace = time_now;
				traceid.clear();
				fmt::format_to(std::back_inserter(traceid), "makotrace{:0>19d}", total_xacts);
				logr.debug("txn tracing {}", traceid);
				auto err = Error{};
				err = tx.setOptionNothrow(FDB_TR_OPTION_DEBUG_TRANSACTION_IDENTIFIER, toBytesRef(traceid));
				if (err) {
					logr.error("TR_OPTION_DEBUG_TRANSACTION_IDENTIFIER: {}", err.what());
				}
				err = tx.setOptionNothrow(FDB_TR_OPTION_LOG_TRANSACTION, BytesRef());
				if (err) {
					logr.error("TR_OPTION_LOG_TRANSACTION: {}", err.what());
				}
			}
		}

		/* enable transaction tagging */
		if (dotagging > 0) {
			tagstr.clear();
			fmt::format_to(std::back_inserter(tagstr),
			               "{}{}{:0>3d}",
			               KEY_PREFIX,
			               args.txntagging_prefix,
			               urand(0, args.txntagging - 1));
			auto err = tx.setOptionNothrow(FDB_TR_OPTION_AUTO_THROTTLE_TAG, toBytesRef(tagstr));
			if (err) {
				logr.error("TR_OPTION_DEBUG_TRANSACTION_IDENTIFIER: {}", err.what());
			}
		}

		rc = runOneTask(tx, args, stats, sample_bins);
		if (rc) {
			logr.warn("runOneTask failed ({})", rc);
		}

		if (thread_iters != -1) {
			if (thread_iters >= xacts) {
				/* xact limit reached */
				break;
			}
		} else if (signal.load() == SIGNAL_RED) {
			/* signal turned red, target duration reached */
			break;
		}
		xacts++;
		total_xacts++;
	}
	return rc;
}

std::string getStatsFilename(std::string_view dirname, int worker_id, int thread_id, int op) {

	return fmt::format("{}/{}_{}_{}", dirname, worker_id + 1, thread_id + 1, opTable[op].name());
}

void dumpThreadSamples(Arguments const& args,
                       pid_t parent_id,
                       int worker_id,
                       int thread_id,
                       const LatencySampleBinArray& sample_bins,
                       bool overwrite = true) {
	const auto dirname = fmt::format("{}{}", TEMP_DATA_STORE, parent_id);
	const auto rc = mkdir(dirname.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	if (rc < 0 && errno != EEXIST) {
		logr.error("mkdir {}: {}", dirname, strerror(errno));
		return;
	}
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			const auto filename = getStatsFilename(dirname, worker_id, thread_id, op);
			auto fp = fopen(filename.c_str(), overwrite ? "w" : "a");
			if (!fp) {
				logr.error("fopen({}): {}", filename, strerror(errno));
				continue;
			}
			auto fclose_guard = ExitGuard([fp]() { fclose(fp); });
			sample_bins[op].forEachBlock([fp](auto ptr, auto count) { fwrite(ptr, sizeof(*ptr) * count, 1, fp); });
		}
	}
}

void runAsyncWorkload(Arguments const& args,
                      pid_t pid_main,
                      int worker_id,
                      shared_memory::Access shm,
                      boost::asio::io_context& io_context,
                      std::vector<Database>& databases) {
	auto dump_samples = [&args, pid_main, worker_id](auto&& states) {
		auto overwrite = true; /* overwrite or append */
		for (const auto& state : states) {
			dumpThreadSamples(args, pid_main, worker_id, 0 /*thread_id*/, state->sample_bins, overwrite);
			overwrite = false;
		}
	};
	auto stopcount = std::atomic<int>{};
	if (args.mode == MODE_BUILD) {
		auto states = std::vector<PopulateStateHandle>(args.async_xacts);
		for (auto i = 0; i < args.async_xacts; i++) {
			const auto key_begin = insertBegin(args.rows, worker_id, i, args.num_processes, args.async_xacts);
			const auto key_end = insertEnd(args.rows, worker_id, i, args.num_processes, args.async_xacts);
			auto db = databases[i % args.num_databases];
			auto state =
			    std::make_shared<ResumableStateForPopulate>(Logger(WorkerProcess{}, args.verbose, worker_id, i),
			                                                db,
			                                                db.createTransaction(),
			                                                io_context,
			                                                args,
			                                                shm.statsSlot(worker_id, i),
			                                                stopcount,
			                                                key_begin,
			                                                key_end);
			state->watch_tx.start();
			state->watch_total.start();
			states[i] = state;
		}
		while (shm.headerConst().signal.load() != SIGNAL_GREEN)
			usleep(1000);
		// launch [async_xacts] concurrent transactions
		for (auto state : states)
			state->postNextTick();
		while (stopcount.load() != args.async_xacts)
			usleep(1000);
		dump_samples(states);
	} else if (args.mode == MODE_RUN) {
		auto states = std::vector<RunWorkloadStateHandle>(args.async_xacts);
		for (auto i = 0; i < args.async_xacts; i++) {
			auto db = databases[i % args.num_databases];
			const auto max_iters =
			    args.iteration == 0
			        ? -1
			        : computeThreadIters(args.iteration, worker_id, i, args.num_processes, args.async_xacts);
			auto state =
			    std::make_shared<ResumableStateForRunWorkload>(Logger(WorkerProcess{}, args.verbose, worker_id, i),
			                                                   db,
			                                                   db.createTransaction(),
			                                                   io_context,
			                                                   args,
			                                                   shm.statsSlot(worker_id, i),
			                                                   stopcount,
			                                                   shm.headerConst().signal,
			                                                   max_iters,
			                                                   getOpBegin(args));
			states[i] = state;
			state->watch_task.start();
			state->watch_tx.start();
		}
		while (shm.headerConst().signal.load() != SIGNAL_GREEN)
			usleep(1000);
		for (auto state : states)
			state->postNextTick();
		logr.debug("Launched {} concurrent transactions", states.size());
		while (stopcount.load() != args.async_xacts)
			usleep(1000);
		logr.debug("All transactions completed");
		dump_samples(states);
	}
}

/* mako worker thread */
void workerThread(ThreadArgs& thread_args) {
	const auto& args = *thread_args.args;
	const auto parent_id = thread_args.parent_id;
	const auto worker_id = thread_args.worker_id;
	const auto thread_id = thread_args.thread_id;
	const auto dotrace = (worker_id == 0 && thread_id == 0 && args.txntrace) ? args.txntrace : 0;
	auto database = thread_args.database;
	const auto dotagging = args.txntagging;
	const auto& signal = thread_args.shm.headerConst().signal;
	const auto& throttle_factor = thread_args.shm.headerConst().throttle_factor;
	auto& readycount = thread_args.shm.header().readycount;
	auto& stopcount = thread_args.shm.header().stopcount;
	auto& stats = thread_args.shm.statsSlot(worker_id, thread_id);
	logr = Logger(WorkerProcess{}, args.verbose, worker_id, thread_id);

	logr.debug("started, tid: {}", reinterpret_cast<uint64_t>(pthread_self()));

	const auto thread_tps =
	    args.tpsmax == 0 ? 0
	                     : computeThreadTps(args.tpsmax, worker_id, thread_id, args.num_processes, args.num_threads);

	const auto thread_iters =
	    args.iteration == 0
	        ? -1
	        : computeThreadIters(args.iteration, worker_id, thread_id, args.num_processes, args.num_threads);

	/* create my own transaction object */
	auto tx = database.createTransaction();

	/* i'm ready */
	readycount.fetch_add(1);
	auto stopcount_guard = ExitGuard([&stopcount]() { stopcount.fetch_add(1); });
	while (signal.load() == SIGNAL_OFF) {
		usleep(10000); /* 10ms */
	}

	auto& sample_bins = thread_args.sample_bins;

	if (args.mode == MODE_CLEAN) {
		auto rc = cleanup(tx, args);
		if (rc < 0) {
			logr.error("cleanup failed");
		}
	} else if (args.mode == MODE_BUILD) {
		auto rc = populate(tx, args, worker_id, thread_id, thread_tps, stats, sample_bins);
		if (rc < 0) {
			logr.error("populate failed");
		}
	} else if (args.mode == MODE_RUN) {
		auto rc = runWorkload(
		    tx, args, thread_tps, throttle_factor, thread_iters, signal, stats, sample_bins, dotrace, dotagging);
		if (rc < 0) {
			logr.error("runWorkload failed");
		}
	}

	if (args.mode == MODE_BUILD || args.mode == MODE_RUN) {
		dumpThreadSamples(args, parent_id, worker_id, thread_id, sample_bins);
	}
}

/* mako worker process */
int workerProcessMain(Arguments const& args, int worker_id, shared_memory::Access shm, pid_t pid_main) {
	logr.debug("started");

	auto err = Error{};
	/* Everything starts from here */

	selectApiVersion(args.api_version);

	/* enable flatbuffers if specified */
	if (args.flatbuffers) {
#ifdef FDB_NET_OPTION_USE_FLATBUFFERS
		logr.debug("Using flatbuffers");
		err = network::setOptionNothrow(FDB_NET_OPTION_USE_FLATBUFFERS,
		                                BytesRef(&args.flatbuffers, sizeof(args.flatbuffers)));
		if (err) {
			logr.error("network::setOption(USE_FLATBUFFERS): {}", err.what());
		}
#else
		logr.info("flatbuffers is not supported in FDB API version {}", FDB_API_VERSION);
#endif
	}

	/* Set client logr group */
	if (args.log_group[0] != '\0') {
		err = network::setOptionNothrow(FDB_NET_OPTION_TRACE_LOG_GROUP, BytesRef(toBytePtr(args.log_group)));
		if (err) {
			logr.error("network::setOption(FDB_NET_OPTION_TRACE_LOG_GROUP): {}", err.what());
		}
	}

	/* enable tracing if specified */
	if (args.trace) {
		logr.debug("Enable Tracing in {} ({})",
		           (args.traceformat == 0) ? "XML" : "JSON",
		           (args.tracepath[0] == '\0') ? "current directory" : args.tracepath);
		err = network::setOptionNothrow(FDB_NET_OPTION_TRACE_ENABLE, BytesRef(toBytePtr(args.tracepath)));
		if (err) {
			logr.error("network::setOption(TRACE_ENABLE): {}", err.what());
		}
		if (args.traceformat == 1) {
			err = network::setOptionNothrow(FDB_NET_OPTION_TRACE_FORMAT, BytesRef(toBytePtr("json")));
			if (err) {
				logr.error("network::setOption(FDB_NET_OPTION_TRACE_FORMAT): {}", err.what());
			}
		}
	}

	/* enable knobs if specified */
	if (args.knobs[0] != '\0') {
		auto knobs = std::string_view(args.knobs);
		const auto delim = std::string_view(", ");
		while (true) {
			knobs.remove_prefix(std::min(knobs.find_first_not_of(delim), knobs.size()));
			auto knob = knobs.substr(0, knobs.find_first_of(delim));
			if (knob.empty())
				break;
			logr.debug("Setting client knob: {}", knob);
			err = network::setOptionNothrow(FDB_NET_OPTION_KNOB, toBytesRef(knob));
			if (err) {
				logr.error("network::setOption({}): {}", knob, err.what());
			}
			knobs.remove_prefix(knob.size());
		}
	}

	if (args.client_threads_per_version > 0) {
		err = network::setOptionNothrow(FDB_NET_OPTION_CLIENT_THREADS_PER_VERSION, args.client_threads_per_version);
		if (err) {
			logr.error("network::setOption (FDB_NET_OPTION_CLIENT_THREADS_PER_VERSION) ({}): {}",
			           args.client_threads_per_version,
			           err.what());
			// let's exit here since we do not want to confuse users
			// that mako is running with multi-threaded client enabled
			return -1;
		}
	}

	/* Network thread must be setup before doing anything */
	logr.debug("network::setup()");
	network::setup();

	/* Each worker process will have its own network thread */
	logr.debug("creating network thread");
	auto network_thread = std::thread([parent_logr = logr]() {
		logr = parent_logr;
		logr.debug("network thread started");
		if (auto err = network::run()) {
			logr.error("network::run(): {}", err.what());
		}
	});

	/*** let's party! ***/

	auto databases = std::vector<fdb::Database>(args.num_databases);
	/* set up database for worker threads */
	for (auto i = 0; i < args.num_databases; i++) {
		size_t cluster_index = args.num_fdb_clusters <= 1 ? 0 : i % args.num_fdb_clusters;
		databases[i] = Database(args.cluster_files[cluster_index]);
		logr.debug("creating database at cluster {}", args.cluster_files[cluster_index]);
		if (args.disable_ryw) {
			databases[i].setOption(FDB_DB_OPTION_SNAPSHOT_RYW_DISABLE, BytesRef{});
		}
	}

	if (!args.async_xacts) {
		logr.debug("creating {} worker threads", args.num_threads);
		auto worker_threads = std::vector<std::thread>(args.num_threads);

		/* spawn worker threads */
		auto thread_args = std::vector<ThreadArgs>(args.num_threads);

		for (auto i = 0; i < args.num_threads; i++) {
			auto& this_args = thread_args[i];
			this_args.worker_id = worker_id;
			this_args.thread_id = i;
			this_args.parent_id = pid_main;
			this_args.args = &args;
			this_args.shm = shm;
			this_args.database = databases[i % args.num_databases];

			/* for ops to run, pre-allocate one latency sample block */
			for (auto op = 0; op < MAX_OP; op++) {
				if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
					this_args.sample_bins[op].reserveOneBlock();
				}
			}
			worker_threads[i] = std::thread(workerThread, std::ref(this_args));
		}
		/* wait for everyone to finish */
		for (auto i = 0; i < args.num_threads; i++) {
			logr.debug("waiting for worker thread {} to join", i + 1);
			worker_threads[i].join();
		}
	} else {
		logr.debug("running async mode with {} concurrent transactions", args.async_xacts);
		auto ctx = boost::asio::io_context{};
		using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
		auto wg = WorkGuard(ctx.get_executor());
		auto worker_threads = std::vector<std::thread>(args.num_threads);
		for (auto i = 0; i < args.num_threads; i++) {
			worker_threads[i] = std::thread([&ctx, &args, worker_id, i]() {
				logr = Logger(WorkerProcess{}, args.verbose, worker_id);
				logr.debug("Async-mode worker thread {} started", i + 1);
				ctx.run();
				logr.debug("Async-mode worker thread {} finished", i + 1);
			});
		}
		shm.header().readycount.fetch_add(args.num_threads);
		runAsyncWorkload(args, pid_main, worker_id, shm, ctx, databases);
		wg.reset();
		for (auto& thread : worker_threads)
			thread.join();
		shm.header().stopcount.fetch_add(args.num_threads);
	}

	/* stop the network thread */
	logr.debug("network::stop()");
	err = network::stop();
	if (err) {
		logr.error("network::stop(): {}", err.what());
	}

	/* wait for the network thread to join */
	logr.debug("waiting for network thread to join");
	network_thread.join();

	return 0;
}

/* initialize the parameters with default values */
int initArguments(Arguments& args) {
	memset(&args, 0, sizeof(Arguments)); /* zero-out everything */
	args.num_fdb_clusters = 0;
	args.num_databases = 1;
	args.api_version = maxApiVersion();
	args.json = 0;
	args.num_processes = 1;
	args.num_threads = 1;
	args.async_xacts = 0;
	args.mode = MODE_INVALID;
	args.rows = 100000;
	args.row_digits = digits(args.rows);
	args.seconds = 30;
	args.iteration = 0;
	args.tpsmax = 0;
	args.tpsmin = -1;
	args.tpsinterval = 10;
	args.tpschange = TPS_SIN;
	args.sampling = 1000;
	args.key_length = 32;
	args.value_length = 16;
	args.zipf = 0;
	args.commit_get = 0;
	args.verbose = 1;
	args.flatbuffers = 0; /* internal */
	args.knobs[0] = '\0';
	args.log_group[0] = '\0';
	args.prefixpadding = 0;
	args.trace = 0;
	args.tracepath[0] = '\0';
	args.traceformat = 0; /* default to client's default (XML) */
	args.streaming_mode = FDB_STREAMING_MODE_WANT_ALL;
	args.txntrace = 0;
	args.txntagging = 0;
	memset(args.txntagging_prefix, 0, TAGPREFIXLENGTH_MAX);
	for (auto i = 0; i < MAX_OP; i++) {
		args.txnspec.ops[i][OP_COUNT] = 0;
	}
	args.client_threads_per_version = 0;
	args.disable_ryw = 0;
	args.json_output_path[0] = '\0';
	args.bg_materialize_files = false;
	args.bg_file_path[0] = '\0';
	return 0;
}

/* parse transaction specification */
int parseTransaction(Arguments& args, char const* optarg) {
	char const* ptr = optarg;
	int op = 0;
	int rangeop = 0;
	int num;
	int error = 0;

	for (op = 0; op < MAX_OP; op++) {
		args.txnspec.ops[op][OP_COUNT] = 0;
		args.txnspec.ops[op][OP_RANGE] = 0;
	}

	op = 0;
	while (*ptr) {
// Clang gives false positive array bounds warning, which must be ignored:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warray-bounds"
		if (strncmp(ptr, "grv", 3) == 0) {
			op = OP_GETREADVERSION;
			ptr += 3;
		} else if (strncmp(ptr, "gr", 2) == 0) {
			op = OP_GETRANGE;
			rangeop = 1;
			ptr += 2;
		} else if (strncmp(ptr, "g", 1) == 0) {
			op = OP_GET;
			ptr++;
		} else if (strncmp(ptr, "sgr", 3) == 0) {
			op = OP_SGETRANGE;
			rangeop = 1;
			ptr += 3;
		} else if (strncmp(ptr, "sg", 2) == 0) {
			op = OP_SGET;
			ptr += 2;
		} else if (strncmp(ptr, "u", 1) == 0) {
			op = OP_UPDATE;
			ptr++;
		} else if (strncmp(ptr, "ir", 2) == 0) {
			op = OP_INSERTRANGE;
			rangeop = 1;
			ptr += 2;
		} else if (strncmp(ptr, "i", 1) == 0) {
			op = OP_INSERT;
			ptr++;
		} else if (strncmp(ptr, "cr", 2) == 0) {
			op = OP_CLEARRANGE;
			rangeop = 1;
			ptr += 2;
		} else if (strncmp(ptr, "c", 1) == 0) {
			op = OP_CLEAR;
			ptr++;
		} else if (strncmp(ptr, "scr", 3) == 0) {
			op = OP_SETCLEARRANGE;
			rangeop = 1;
			ptr += 3;
		} else if (strncmp(ptr, "sc", 2) == 0) {
			op = OP_SETCLEAR;
			ptr += 2;
		} else if (strncmp(ptr, "bg", 2) == 0) {
			op = OP_READ_BG;
			rangeop = 1;
			ptr += 2;
		} else {
			logr.error("Invalid transaction spec: {}", ptr);
			error = 1;
			break;
		}
#pragma clang diagnostic pop

		/* count */
		num = 0;
		if ((*ptr < '0') || (*ptr > '9')) {
			num = 1; /* if omitted, set it to 1 */
		} else {
			while ((*ptr >= '0') && (*ptr <= '9')) {
				num = num * 10 + *ptr - '0';
				ptr++;
			}
		}
		/* set count */
		args.txnspec.ops[op][OP_COUNT] = num;

		if (rangeop) {
			if (*ptr != ':') {
				error = 1;
				break;
			} else {
				ptr++; /* skip ':' */
				/* check negative '-' sign */
				if (*ptr == '-') {
					args.txnspec.ops[op][OP_REVERSE] = 1;
					ptr++;
				} else {
					args.txnspec.ops[op][OP_REVERSE] = 0;
				}
				num = 0;
				if ((*ptr < '0') || (*ptr > '9')) {
					error = 1;
					break;
				}
				while ((*ptr >= '0') && (*ptr <= '9')) {
					num = num * 10 + *ptr - '0';
					ptr++;
				}
				/* set range */
				args.txnspec.ops[op][OP_RANGE] = num;
			}
		}
		rangeop = 0;
	}

	if (error) {
		logr.error("invalid transaction specification {}", optarg);
		return -1;
	}

	if (args.verbose == VERBOSE_DEBUG) {
		for (op = 0; op < MAX_OP; op++) {
			logr.debug("OP: {}: {}: {}", op, args.txnspec.ops[op][0], args.txnspec.ops[op][1]);
		}
	}

	return 0;
}

void usage() {
	printf("Usage:\n");
	printf("%-24s %s\n", "-h, --help", "Print this message");
	printf("%-24s %s\n", "    --version", "Print FDB version");
	printf("%-24s %s\n", "-v, --verbose", "Specify verbosity");
	printf("%-24s %s\n", "-a, --api_version=API_VERSION", "Specify API_VERSION to use");
	printf("%-24s %s\n", "-c, --cluster=FILE", "Specify FDB cluster file");
	printf("%-24s %s\n", "-d, --num_databases=NUM_DATABASES", "Specify number of databases");
	printf("%-24s %s\n", "-p, --procs=PROCS", "Specify number of worker processes");
	printf("%-24s %s\n", "-t, --threads=THREADS", "Specify number of worker threads");
	printf("%-24s %s\n", "    --async_xacts", "Specify number of concurrent transactions to be run in async mode");
	printf("%-24s %s\n", "-r, --rows=ROWS", "Specify number of records");
	printf("%-24s %s\n", "-s, --seconds=SECONDS", "Specify the test duration in seconds\n");
	printf("%-24s %s\n", "", "This option cannot be specified with --iteration.");
	printf("%-24s %s\n", "-i, --iteration=ITERS", "Specify the number of iterations.\n");
	printf("%-24s %s\n", "", "This option cannot be specified with --seconds.");
	printf("%-24s %s\n", "    --keylen=LENGTH", "Specify the key lengths");
	printf("%-24s %s\n", "    --vallen=LENGTH", "Specify the value lengths");
	printf("%-24s %s\n", "-x, --transaction=SPEC", "Transaction specification");
	printf("%-24s %s\n", "    --tps|--tpsmax=TPS", "Specify the target max TPS");
	printf("%-24s %s\n", "    --tpsmin=TPS", "Specify the target min TPS");
	printf("%-24s %s\n", "    --tpsinterval=SEC", "Specify the TPS change interval (Default: 10 seconds)");
	printf("%-24s %s\n", "    --tpschange=<sin|square|pulse>", "Specify the TPS change type (Default: sin)");
	printf("%-24s %s\n", "    --sampling=RATE", "Specify the sampling rate for latency stats");
	printf("%-24s %s\n", "-m, --mode=MODE", "Specify the mode (build, run, clean)");
	printf("%-24s %s\n", "-z, --zipf", "Use zipfian distribution instead of uniform distribution");
	printf("%-24s %s\n", "    --commitget", "Commit GETs");
	printf("%-24s %s\n", "    --loggroup=LOGGROUP", "Set client logr group");
	printf("%-24s %s\n", "    --prefix_padding", "Pad key by prefixing data (Default: postfix padding)");
	printf("%-24s %s\n", "    --trace", "Enable tracing");
	printf("%-24s %s\n", "    --tracepath=PATH", "Set trace file path");
	printf("%-24s %s\n", "    --trace_format <xml|json>", "Set trace format (Default: json)");
	printf("%-24s %s\n", "    --txntrace=sec", "Specify transaction tracing interval (Default: 0)");
	printf(
	    "%-24s %s\n", "    --txntagging", "Specify the number of different transaction tag (Default: 0, max = 1000)");
	printf("%-24s %s\n",
	       "    --txntagging_prefix",
	       "Specify the prefix of transaction tag - mako${txntagging_prefix} (Default: '')");
	printf("%-24s %s\n", "    --knobs=KNOBS", "Set client knobs");
	printf("%-24s %s\n", "    --flatbuffers", "Use flatbuffers");
	printf("%-24s %s\n", "    --streaming", "Streaming mode: all (default), iterator, small, medium, large, serial");
	printf("%-24s %s\n", "    --disable_ryw", "Disable snapshot read-your-writes");
	printf("%-24s %s\n", "    --json_report=PATH", "Output stats to the specified json file (Default: mako.json)");
	printf("%-24s %s\n",
	       "    --bg_file_path=PATH",
	       "Read blob granule files from the local filesystem at PATH and materialize the results.");
}

/* parse benchmark paramters */
int parseArguments(int argc, char* argv[], Arguments& args) {
	int rc;
	int c;
	int idx;
	while (1) {
		const char* short_options = "a:c:d:p:t:r:s:i:x:v:m:hz";
		static struct option long_options[] = {
			/* name, has_arg, flag, val */
			{ "api_version", required_argument, NULL, 'a' },
			{ "cluster", required_argument, NULL, 'c' },
			{ "num_databases", optional_argument, NULL, 'd' },
			{ "procs", required_argument, NULL, 'p' },
			{ "threads", required_argument, NULL, 't' },
			{ "async_xacts", required_argument, NULL, ARG_ASYNC },
			{ "rows", required_argument, NULL, 'r' },
			{ "seconds", required_argument, NULL, 's' },
			{ "iteration", required_argument, NULL, 'i' },
			{ "keylen", required_argument, NULL, ARG_KEYLEN },
			{ "vallen", required_argument, NULL, ARG_VALLEN },
			{ "transaction", required_argument, NULL, 'x' },
			{ "tps", required_argument, NULL, ARG_TPS },
			{ "tpsmax", required_argument, NULL, ARG_TPSMAX },
			{ "tpsmin", required_argument, NULL, ARG_TPSMIN },
			{ "tpsinterval", required_argument, NULL, ARG_TPSINTERVAL },
			{ "tpschange", required_argument, NULL, ARG_TPSCHANGE },
			{ "sampling", required_argument, NULL, ARG_SAMPLING },
			{ "verbose", required_argument, NULL, 'v' },
			{ "mode", required_argument, NULL, 'm' },
			{ "knobs", required_argument, NULL, ARG_KNOBS },
			{ "loggroup", required_argument, NULL, ARG_LOGGROUP },
			{ "tracepath", required_argument, NULL, ARG_TRACEPATH },
			{ "trace_format", required_argument, NULL, ARG_TRACEFORMAT },
			{ "streaming", required_argument, NULL, ARG_STREAMING_MODE },
			{ "txntrace", required_argument, NULL, ARG_TXNTRACE },
			/* no args */
			{ "help", no_argument, NULL, 'h' },
			{ "zipf", no_argument, NULL, 'z' },
			{ "commitget", no_argument, NULL, ARG_COMMITGET },
			{ "flatbuffers", no_argument, NULL, ARG_FLATBUFFERS },
			{ "prefix_padding", no_argument, NULL, ARG_PREFIXPADDING },
			{ "trace", no_argument, NULL, ARG_TRACE },
			{ "txntagging", required_argument, NULL, ARG_TXNTAGGING },
			{ "txntagging_prefix", required_argument, NULL, ARG_TXNTAGGINGPREFIX },
			{ "version", no_argument, NULL, ARG_VERSION },
			{ "client_threads_per_version", required_argument, NULL, ARG_CLIENT_THREADS_PER_VERSION },
			{ "disable_ryw", no_argument, NULL, ARG_DISABLE_RYW },
			{ "json_report", optional_argument, NULL, ARG_JSON_REPORT },
			{ "bg_file_path", required_argument, NULL, ARG_BG_FILE_PATH },
			{ NULL, 0, NULL, 0 }
		};
		idx = 0;
		c = getopt_long(argc, argv, short_options, long_options, &idx);
		if (c < 0) {
			break;
		}
		switch (c) {
		case '?':
		case 'h':
			usage();
			return -1;
		case 'a':
			args.api_version = atoi(optarg);
			break;
		case 'c': {
			const char delim[] = ",";
			char* cluster_file = strtok(optarg, delim);
			while (cluster_file != NULL) {
				strcpy(args.cluster_files[args.num_fdb_clusters++], cluster_file);
				cluster_file = strtok(NULL, delim);
			}
			break;
		}
		case 'd':
			args.num_databases = atoi(optarg);
			break;
		case 'p':
			args.num_processes = atoi(optarg);
			break;
		case 't':
			args.num_threads = atoi(optarg);
			break;
		case 'r':
			args.rows = atoi(optarg);
			args.row_digits = digits(args.rows);
			break;
		case 's':
			args.seconds = atoi(optarg);
			break;
		case 'i':
			args.iteration = atoi(optarg);
			break;
		case 'x':
			rc = parseTransaction(args, optarg);
			if (rc < 0)
				return -1;
			break;
		case 'v':
			args.verbose = atoi(optarg);
			break;
		case 'z':
			args.zipf = 1;
			break;
		case 'm':
			if (strcmp(optarg, "clean") == 0) {
				args.mode = MODE_CLEAN;
			} else if (strcmp(optarg, "build") == 0) {
				args.mode = MODE_BUILD;
			} else if (strcmp(optarg, "run") == 0) {
				args.mode = MODE_RUN;
			}
			break;
		case ARG_ASYNC:
			args.async_xacts = atoi(optarg);
			break;
		case ARG_KEYLEN:
			args.key_length = atoi(optarg);
			break;
		case ARG_VALLEN:
			args.value_length = atoi(optarg);
			break;
		case ARG_TPS:
		case ARG_TPSMAX:
			args.tpsmax = atoi(optarg);
			break;
		case ARG_TPSMIN:
			args.tpsmin = atoi(optarg);
			break;
		case ARG_TPSINTERVAL:
			args.tpsinterval = atoi(optarg);
			break;
		case ARG_TPSCHANGE:
			if (strcmp(optarg, "sin") == 0)
				args.tpschange = TPS_SIN;
			else if (strcmp(optarg, "square") == 0)
				args.tpschange = TPS_SQUARE;
			else if (strcmp(optarg, "pulse") == 0)
				args.tpschange = TPS_PULSE;
			else {
				logr.error("--tpschange must be sin, square or pulse");
				return -1;
			}
			break;
		case ARG_SAMPLING:
			args.sampling = atoi(optarg);
			break;
		case ARG_VERSION:
			logr.error("Version: {}", FDB_API_VERSION);
			exit(0);
			break;
		case ARG_COMMITGET:
			args.commit_get = 1;
			break;
		case ARG_FLATBUFFERS:
			args.flatbuffers = 1;
			break;
		case ARG_KNOBS:
			memcpy(args.knobs, optarg, strlen(optarg) + 1);
			break;
		case ARG_LOGGROUP:
			memcpy(args.log_group, optarg, strlen(optarg) + 1);
			break;
		case ARG_PREFIXPADDING:
			args.prefixpadding = 1;
			break;
		case ARG_TRACE:
			args.trace = 1;
			break;
		case ARG_TRACEPATH:
			args.trace = 1;
			memcpy(args.tracepath, optarg, strlen(optarg) + 1);
			break;
		case ARG_TRACEFORMAT:
			if (strncmp(optarg, "json", 5) == 0) {
				args.traceformat = 1;
			} else if (strncmp(optarg, "xml", 4) == 0) {
				args.traceformat = 0;
			} else {
				logr.error("Invalid trace_format {}", optarg);
				return -1;
			}
			break;
		case ARG_STREAMING_MODE:
			if (strncmp(optarg, "all", 3) == 0) {
				args.streaming_mode = FDB_STREAMING_MODE_WANT_ALL;
			} else if (strncmp(optarg, "iterator", 8) == 0) {
				args.streaming_mode = FDB_STREAMING_MODE_ITERATOR;
			} else if (strncmp(optarg, "small", 5) == 0) {
				args.streaming_mode = FDB_STREAMING_MODE_SMALL;
			} else if (strncmp(optarg, "medium", 6) == 0) {
				args.streaming_mode = FDB_STREAMING_MODE_MEDIUM;
			} else if (strncmp(optarg, "large", 5) == 0) {
				args.streaming_mode = FDB_STREAMING_MODE_LARGE;
			} else if (strncmp(optarg, "serial", 6) == 0) {
				args.streaming_mode = FDB_STREAMING_MODE_SERIAL;
			} else {
				logr.error("Invalid streaming mode {}", optarg);
				return -1;
			}
			break;
		case ARG_TXNTRACE:
			args.txntrace = atoi(optarg);
			break;

		case ARG_TXNTAGGING:
			args.txntagging = atoi(optarg);
			if (args.txntagging > 1000) {
				args.txntagging = 1000;
			}
			break;
		case ARG_TXNTAGGINGPREFIX:
			if (strlen(optarg) > TAGPREFIXLENGTH_MAX) {
				logr.error("the length of txntagging_prefix is larger than {}", TAGPREFIXLENGTH_MAX);
				exit(0);
			}
			memcpy(args.txntagging_prefix, optarg, strlen(optarg));
			break;
		case ARG_CLIENT_THREADS_PER_VERSION:
			args.client_threads_per_version = atoi(optarg);
			break;
		case ARG_DISABLE_RYW:
			args.disable_ryw = 1;
			break;
		case ARG_JSON_REPORT:
			if (optarg == NULL && (argv[optind] == NULL || (argv[optind] != NULL && argv[optind][0] == '-'))) {
				// if --report_json is the last option and no file is specified
				// or --report_json is followed by another option
				char default_file[] = "mako.json";
				strncpy(args.json_output_path, default_file, sizeof(default_file));
			} else {
				strncpy(args.json_output_path, optarg, std::min(sizeof(args.json_output_path), strlen(optarg) + 1));
			}
			break;
		case ARG_BG_FILE_PATH:
			args.bg_materialize_files = true;
			strncpy(args.bg_file_path, optarg, std::min(sizeof(args.bg_file_path), strlen(optarg) + 1));
		}
	}

	if ((args.tpsmin == -1) || (args.tpsmin > args.tpsmax)) {
		args.tpsmin = args.tpsmax;
	}

	return 0;
}

int validateArguments(Arguments const& args) {
	if (args.mode == MODE_INVALID) {
		logr.error("--mode has to be set");
		return -1;
	}
	if (args.verbose < VERBOSE_NONE || args.verbose > VERBOSE_DEBUG) {
		logr.error("--verbose must be between 0 and 3");
		return -1;
	}
	if (args.rows <= 0) {
		logr.error("--rows must be a positive integer");
		return -1;
	}
	if (args.key_length < 0) {
		logr.error("--keylen must be a positive integer");
		return -1;
	}
	if (args.value_length < 0) {
		logr.error("--vallen must be a positive integer");
		return -1;
	}
	if (args.num_fdb_clusters > NUM_CLUSTERS_MAX) {
		logr.error("Mako is not supported to do work to more than {} clusters", NUM_CLUSTERS_MAX);
		return -1;
	}
	if (args.num_databases > NUM_DATABASES_MAX) {
		logr.error("Mako is not supported to do work to more than {} databases", NUM_DATABASES_MAX);
		return -1;
	}
	if (args.num_databases < args.num_fdb_clusters) {
		logr.error("--num_databases ({}) must be >= number of clusters({})", args.num_databases, args.num_fdb_clusters);
		return -1;
	}
	if (args.num_threads < args.num_databases) {
		logr.error("--threads ({}) must be >= number of databases ({})", args.num_threads, args.num_databases);
		return -1;
	}
	if (args.key_length < 4 /* "mako" */ + args.row_digits) {
		logr.error("--keylen must be larger than {} to store \"mako\" prefix "
		           "and maximum row number",
		           4 + args.row_digits);
		return -1;
	}
	if (args.mode == MODE_RUN) {
		if ((args.seconds > 0) && (args.iteration > 0)) {
			logr.error("Cannot specify seconds and iteration together");
			return -1;
		}
		if ((args.seconds == 0) && (args.iteration == 0)) {
			logr.error("Must specify either seconds or iteration");
			return -1;
		}
		if (args.txntagging < 0) {
			logr.error("--txntagging must be a non-negative integer");
			return -1;
		}
	}
	return 0;
}

void printStats(Arguments const& args, ThreadStatistics const* stats, double const duration_sec, FILE* fp) {
	static ThreadStatistics prev;

	const auto num_effective_threads = args.async_xacts > 0 ? args.async_xacts : args.num_threads;
	auto current = ThreadStatistics{};
	for (auto i = 0; i < args.num_processes; i++) {
		for (auto j = 0; j < num_effective_threads; j++) {
			current.combine(stats[(i * num_effective_threads) + j]);
		}
	}

	if (fp) {
		fwrite("{", 1, 1, fp);
	}
	putTitleRight("OPS");
	auto print_err = false;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0) {
			const auto ops_total_diff = current.getOpCount(op) - prev.getOpCount(op);
			putField(ops_total_diff);
			if (fp) {
				fmt::print(fp, "\"{}\": {},", getOpName(op), ops_total_diff);
			}
			print_err = print_err || (current.getErrorCount(op) - prev.getErrorCount(op)) > 0;
		}
	}
	/* TPS */
	const auto tps = (current.getOpCount(OP_TASK) - prev.getOpCount(OP_TASK)) / duration_sec;
	putFieldFloat(tps, 2);
	if (fp) {
		fprintf(fp, "\"tps\": %.2f,", tps);
	}

	/* Conflicts */
	const auto conflicts_diff = (current.getConflictCount() - prev.getConflictCount()) / duration_sec;
	putFieldFloat(conflicts_diff, 2);
	fmt::print("\n");
	if (fp) {
		fprintf(fp, "\"conflictsPerSec\": %.2f", conflicts_diff);
	}

	if (print_err) {
		putTitleRight("Errors");
		for (auto op = 0; op < MAX_OP; op++) {
			if (args.txnspec.ops[op][OP_COUNT] > 0) {
				const auto errors_diff = current.getErrorCount(op) - prev.getErrorCount(op);
				putField(errors_diff);
				if (fp) {
					fmt::print(fp, ",\"errors\": {}", errors_diff);
				}
			}
		}
		printf("\n");
	}
	if (fp) {
		fprintf(fp, "}");
	}
	// swap old stats for new
	prev = current;
}

void printStatsHeader(Arguments const& args, bool show_commit, bool is_first_header_empty, bool show_op_stats) {
	/* header */
	if (is_first_header_empty)
		putTitle("");
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0) {
			putField(getOpName(op));
		}
	}

	if (show_commit)
		putField("COMMIT");
	if (show_op_stats) {
		putField("TRANSACTION");
	} else {
		putField("TPS");
		putField("Conflicts/s");
	}
	fmt::print("\n");

	putTitleBar();
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0) {
			putFieldBar();
		}
	}

	/* COMMIT */
	if (show_commit)
		putFieldBar();

	if (show_op_stats) {
		/* TRANSACTION */
		putFieldBar();
	} else {
		/* TPS */
		putFieldBar();

		/* Conflicts */
		putFieldBar();
	}
	fmt::print("\n");
}

void printReport(Arguments const& args,
                 ThreadStatistics const* stats,
                 double const duration_sec,
                 pid_t pid_main,
                 FILE* fp) {

	auto final_stats = ThreadStatistics{};
	const auto num_effective_threads = args.async_xacts > 0 ? args.async_xacts : args.num_threads;
	for (auto i = 0; i < args.num_processes; i++) {
		for (auto j = 0; j < num_effective_threads; j++) {
			const auto idx = i * num_effective_threads + j;
			final_stats.combine(stats[idx]);
		}
	}

	/* overall stats */
	fmt::printf("\n====== Total Duration %6.3f sec ======\n\n", duration_sec);
	fmt::printf("Total Processes:   %8d\n", args.num_processes);
	fmt::printf("Total Threads:     %8d\n", args.num_threads);
	fmt::printf("Total Async Xacts: %8d\n", args.async_xacts);
	if (args.tpsmax == args.tpsmin)
		fmt::printf("Target TPS:        %8d\n", args.tpsmax);
	else {
		fmt::printf("Target TPS (MAX):  %8d\n", args.tpsmax);
		fmt::printf("Target TPS (MIN):  %8d\n", args.tpsmin);
		fmt::printf("TPS Interval:      %8d\n", args.tpsinterval);
		fmt::printf("TPS Change:        ");
		switch (args.tpschange) {
		case TPS_SIN:
			fmt::printf("%8s\n", "SIN");
			break;
		case TPS_SQUARE:
			fmt::printf("%8s\n", "SQUARE");
			break;
		case TPS_PULSE:
			fmt::printf("%8s\n", "PULSE");
			break;
		}
	}
	const auto tps_f = final_stats.getOpCount(OP_TASK) / duration_sec;
	const auto tps_i = static_cast<uint64_t>(tps_f);
	fmt::printf("Total Xacts:       %8lu\n", final_stats.getOpCount(OP_TASK));
	fmt::printf("Total Conflicts:   %8lu\n", final_stats.getConflictCount());
	fmt::printf("Total Errors:      %8lu\n", final_stats.getTotalErrorCount());
	fmt::printf("Overall TPS:       %8lu\n\n", tps_i);

	if (fp) {
		fmt::fprintf(fp, "\"results\": {");
		fmt::fprintf(fp, "\"totalDuration\": %6.3f,", duration_sec);
		fmt::fprintf(fp, "\"totalProcesses\": %d,", args.num_processes);
		fmt::fprintf(fp, "\"totalThreads\": %d,", args.num_threads);
		fmt::fprintf(fp, "\"totalAsyncXacts\": %d,", args.async_xacts);
		fmt::fprintf(fp, "\"targetTPS\": %d,", args.tpsmax);
		fmt::fprintf(fp, "\"totalXacts\": %lu,", final_stats.getOpCount(OP_TASK));
		fmt::fprintf(fp, "\"totalConflicts\": %lu,", final_stats.getConflictCount());
		fmt::fprintf(fp, "\"totalErrors\": %lu,", final_stats.getTotalErrorCount());
		fmt::fprintf(fp, "\"overallTPS\": %lu,", tps_i);
	}

	/* per-op stats */
	printStatsHeader(args, true, true, false);

	/* OPS */
	putTitle("Total OPS");
	if (fp) {
		fmt::fprintf(fp, "\"totalOps\": {");
	}
	auto first_op = true;
	for (auto op = 0; op < MAX_OP; op++) {
		if ((args.txnspec.ops[op][OP_COUNT] > 0 && op != OP_TASK && op != OP_TRANSACTION) || op == OP_COMMIT) {
			putField(final_stats.getOpCount(op));
			if (fp) {
				if (first_op) {
					first_op = 0;
				} else {
					fmt::fprintf(fp, ",");
				}
				fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), final_stats.getOpCount(op));
			}
		}
	}

	/* TPS */
	const auto tps = final_stats.getOpCount(OP_TASK) / duration_sec;
	putFieldFloat(tps, 2);

	/* Conflicts */
	const auto conflicts_rate = final_stats.getConflictCount() / duration_sec;
	putFieldFloat(conflicts_rate, 2);
	fmt::print("\n");

	if (fp) {
		fmt::fprintf(fp, "}, \"tps\": %.2f, \"conflictsPerSec\": %.2f, \"errors\": {", tps, conflicts_rate);
	}

	/* Errors */
	putTitle("Errors");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 && op != OP_TRANSACTION) {
			putField(final_stats.getErrorCount(op));
			if (fp) {
				if (first_op) {
					first_op = 0;
				} else {
					fmt::fprintf(fp, ",");
				}
				fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), final_stats.getErrorCount(op));
			}
		}
	}
	if (fp) {
		fmt::fprintf(fp, "}, \"numSamples\": {");
	}
	fmt::print("\n\n");

	fmt::print("Latency (us)");
	printStatsHeader(args, true, false, true);

	/* Total Samples */
	putTitle("Samples");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			if (final_stats.getLatencyUsTotal(op)) {
				putField(final_stats.getLatencySampleCount(op));
			} else {
				putField("N/A");
			}
			if (fp) {
				if (first_op) {
					first_op = 0;
				} else {
					fmt::fprintf(fp, ",");
				}
				fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), final_stats.getLatencySampleCount(op));
			}
		}
	}
	fmt::print("\n");

	/* Min Latency */
	if (fp) {
		fmt::fprintf(fp, "}, \"minLatency\": {");
	}
	putTitle("Min");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			const auto lat_min = final_stats.getLatencyUsMin(op);
			if (lat_min == -1) {
				putField("N/A");
			} else {
				putField(lat_min);
				if (fp) {
					if (first_op) {
						first_op = 0;
					} else {
						fmt::fprintf(fp, ",");
					}
					fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), lat_min);
				}
			}
		}
	}
	fmt::print("\n");

	/* Avg Latency */
	if (fp) {
		fmt::fprintf(fp, "}, \"avgLatency\": {");
	}
	putTitle("Avg");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			const auto lat_total = final_stats.getLatencyUsTotal(op);
			const auto lat_samples = final_stats.getLatencySampleCount(op);
			if (lat_total) {
				putField(lat_total / lat_samples);
				if (fp) {
					if (first_op) {
						first_op = 0;
					} else {
						fmt::fprintf(fp, ",");
					}
					fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), lat_total / lat_samples);
				}
			} else {
				putField("N/A");
			}
		}
	}
	fmt::printf("\n");

	/* Max Latency */
	if (fp) {
		fmt::fprintf(fp, "}, \"maxLatency\": {");
	}
	putTitle("Max");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			const auto lat_max = final_stats.getLatencyUsMax(op);
			if (lat_max == 0) {
				putField("N/A");
			} else {
				putField(lat_max);
				if (fp) {
					if (first_op) {
						first_op = 0;
					} else {
						fmt::fprintf(fp, ",");
					}
					fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), final_stats.getLatencyUsMax(op));
				}
			}
		}
	}
	fmt::print("\n");

	auto data_points = std::array<std::vector<uint64_t>, MAX_OP>{};

	/* Median Latency */
	if (fp) {
		fmt::fprintf(fp, "}, \"medianLatency\": {");
	}
	putTitle("Median");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			const auto lat_total = final_stats.getLatencyUsTotal(op);
			const auto lat_samples = final_stats.getLatencySampleCount(op);
			data_points[op].reserve(lat_samples);
			if (lat_total && lat_samples) {
				for (auto i = 0; i < args.num_processes; i++) {
					auto load_sample = [pid_main, op, &data_points](int process_id, int thread_id) {
						const auto dirname = fmt::format("{}{}", TEMP_DATA_STORE, pid_main);
						const auto filename = getStatsFilename(dirname, process_id, thread_id, op);
						auto fp = fopen(filename.c_str(), "r");
						if (!fp) {
							logr.error("fopen({}): {}", filename, strerror(errno));
							return;
						}
						auto fclose_guard = ExitGuard([fp]() { fclose(fp); });
						fseek(fp, 0, SEEK_END);
						const auto num_points = ftell(fp) / sizeof(uint64_t);
						fseek(fp, 0, 0);
						for (auto index = 0u; index < num_points; index++) {
							auto value = uint64_t{};
							fread(&value, sizeof(uint64_t), 1, fp);
							data_points[op].push_back(value);
						}
					};
					if (args.async_xacts == 0) {
						for (auto j = 0; j < args.num_threads; j++) {
							load_sample(i, j);
						}
					} else {
						// async mode uses only one file per process
						load_sample(i, 0);
					}
				}
				std::sort(data_points[op].begin(), data_points[op].end());
				const auto num_points = data_points[op].size();
				auto median = uint64_t{};
				if (num_points & 1) {
					median = data_points[op][num_points / 2];
				} else {
					median = (data_points[op][num_points / 2] + data_points[op][num_points / 2 - 1]) >> 1;
				}
				putField(median);
				if (fp) {
					if (first_op) {
						first_op = 0;
					} else {
						fmt::fprintf(fp, ",");
					}
					fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), median);
				}
			} else {
				putField("N/A");
			}
		}
	}
	fmt::print("\n");

	/* 95%ile Latency */
	if (fp) {
		fmt::fprintf(fp, "}, \"p95Latency\": {");
	}
	putTitle("95.0 pctile");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			if (data_points[op].empty() || !final_stats.getLatencyUsTotal(op)) {
				putField("N/A");
				continue;
			}
			const auto num_points = data_points[op].size();
			const auto point_95pct = static_cast<size_t>(std::max(0., (num_points * 0.95) - 1));
			putField(data_points[op][point_95pct]);
			if (fp) {
				if (first_op) {
					first_op = 0;
				} else {
					fmt::fprintf(fp, ",");
				}
				fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), data_points[op][point_95pct]);
			}
		}
	}
	fmt::printf("\n");

	/* 99%ile Latency */
	if (fp) {
		fmt::fprintf(fp, "}, \"p99Latency\": {");
	}
	putTitle("99.0 pctile");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			if (data_points[op].empty() || !final_stats.getLatencyUsTotal(op)) {
				putField("N/A");
				continue;
			}
			const auto num_points = data_points[op].size();
			const auto point_99pct = static_cast<size_t>(std::max(0., (num_points * 0.99) - 1));
			putField(data_points[op][point_99pct]);
			if (fp) {
				if (first_op) {
					first_op = 0;
				} else {
					fmt::fprintf(fp, ",");
				}
				fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), data_points[op][point_99pct]);
			}
		}
	}
	fmt::print("\n");

	/* 99.9%ile Latency */
	if (fp) {
		fmt::fprintf(fp, "}, \"p99.9Latency\": {");
	}
	putTitle("99.9 pctile");
	first_op = 1;
	for (auto op = 0; op < MAX_OP; op++) {
		if (args.txnspec.ops[op][OP_COUNT] > 0 || isAbstractOp(op)) {
			if (data_points[op].empty() || !final_stats.getLatencyUsTotal(op)) {
				putField("N/A");
				continue;
			}
			const auto num_points = data_points[op].size();
			const auto point_99_9pct = static_cast<size_t>(std::max(0., (num_points * 0.999) - 1));
			putField(data_points[op][point_99_9pct]);
			if (fp) {
				if (first_op) {
					first_op = 0;
				} else {
					fmt::fprintf(fp, ",");
				}
				fmt::fprintf(fp, "\"%s\": %lu", getOpName(op), data_points[op][point_99_9pct]);
			}
		}
	}
	fmt::print("\n");
	if (fp) {
		fmt::fprintf(fp, "}}");
	}

	const auto command_remove = fmt::format("rm -rf {}{}", TEMP_DATA_STORE, pid_main);
	system(command_remove.c_str());
}

int statsProcessMain(Arguments const& args,
                     ThreadStatistics const* stats,
                     std::atomic<double>& throttle_factor,
                     std::atomic<int> const& signal,
                     std::atomic<int> const& stopcount,
                     pid_t pid_main) {
	bool first_stats = true;

	/* wait until the signal turn on */
	while (signal.load() == SIGNAL_OFF) {
		usleep(10000); /* 10ms */
	}

	if (args.verbose >= VERBOSE_DEFAULT)
		printStatsHeader(args, false, true, false);

	FILE* fp = NULL;
	if (args.json_output_path[0] != '\0') {
		fp = fopen(args.json_output_path, "w");
		fmt::fprintf(fp, "{\"makoArgs\": {");
		fmt::fprintf(fp, "\"api_version\": %d,", args.api_version);
		fmt::fprintf(fp, "\"json\": %d,", args.json);
		fmt::fprintf(fp, "\"num_processes\": %d,", args.num_processes);
		fmt::fprintf(fp, "\"num_threads\": %d,", args.num_threads);
		fmt::fprintf(fp, "\"async_xacts\": %d,", args.async_xacts);
		fmt::fprintf(fp, "\"mode\": %d,", args.mode);
		fmt::fprintf(fp, "\"rows\": %d,", args.rows);
		fmt::fprintf(fp, "\"seconds\": %d,", args.seconds);
		fmt::fprintf(fp, "\"iteration\": %d,", args.iteration);
		fmt::fprintf(fp, "\"tpsmax\": %d,", args.tpsmax);
		fmt::fprintf(fp, "\"tpsmin\": %d,", args.tpsmin);
		fmt::fprintf(fp, "\"tpsinterval\": %d,", args.tpsinterval);
		fmt::fprintf(fp, "\"tpschange\": %d,", args.tpschange);
		fmt::fprintf(fp, "\"sampling\": %d,", args.sampling);
		fmt::fprintf(fp, "\"key_length\": %d,", args.key_length);
		fmt::fprintf(fp, "\"value_length\": %d,", args.value_length);
		fmt::fprintf(fp, "\"commit_get\": %d,", args.commit_get);
		fmt::fprintf(fp, "\"verbose\": %d,", args.verbose);
		fmt::fprintf(fp, "\"cluster_files\": \"%s\",", args.cluster_files[0]);
		fmt::fprintf(fp, "\"log_group\": \"%s\",", args.log_group);
		fmt::fprintf(fp, "\"prefixpadding\": %d,", args.prefixpadding);
		fmt::fprintf(fp, "\"trace\": %d,", args.trace);
		fmt::fprintf(fp, "\"tracepath\": \"%s\",", args.tracepath);
		fmt::fprintf(fp, "\"traceformat\": %d,", args.traceformat);
		fmt::fprintf(fp, "\"knobs\": \"%s\",", args.knobs);
		fmt::fprintf(fp, "\"flatbuffers\": %d,", args.flatbuffers);
		fmt::fprintf(fp, "\"txntrace\": %d,", args.txntrace);
		fmt::fprintf(fp, "\"txntagging\": %d,", args.txntagging);
		fmt::fprintf(fp, "\"txntagging_prefix\": \"%s\",", args.txntagging_prefix);
		fmt::fprintf(fp, "\"streaming_mode\": %d,", args.streaming_mode);
		fmt::fprintf(fp, "\"disable_ryw\": %d,", args.disable_ryw);
		fmt::fprintf(fp, "\"json_output_path\": \"%s\"", args.json_output_path);
		fmt::fprintf(fp, "},\"samples\": [");
	}

	const auto time_start = steady_clock::now();
	auto time_prev = time_start;
	while (signal.load() != SIGNAL_RED) {
		usleep(100000); /* sleep for 100ms */
		auto time_now = steady_clock::now();

		/* print stats every (roughly) 1 sec */
		if (toDoubleSeconds(time_now - time_prev) >= 1.0) {

			/* adjust throttle rate if needed */
			if (args.tpsmax != args.tpsmin) {
				const auto tpsinterval = static_cast<double>(args.tpsinterval);
				const auto tpsmin = static_cast<double>(args.tpsmin);
				const auto tpsmax = static_cast<double>(args.tpsmax);
				const auto pos = fmod(toDoubleSeconds(time_now - time_start), tpsinterval);
				auto sin_factor = 0.;
				/* set the throttle factor between 0.0 and 1.0 */
				switch (args.tpschange) {
				case TPS_SIN:
					sin_factor = sin(pos / tpsinterval * M_PI * 2.0) / 2.0 + 0.5;
					throttle_factor = 1 - (sin_factor * (1.0 - (tpsmin / tpsmax)));
					break;
				case TPS_SQUARE:
					if (pos < (args.tpsinterval / 2)) {
						/* set to max */
						throttle_factor = 1.0;
					} else {
						/* set to min */
						throttle_factor = tpsmin / tpsmax;
					}
					break;
				case TPS_PULSE:
					if (pos < (1.0 / tpsinterval)) {
						/* set to max */
						throttle_factor = 1.0;
					} else {
						/* set to min */
						throttle_factor = tpsmin / tpsmax;
					}
					break;
				}
			}

			if (args.verbose >= VERBOSE_DEFAULT) {
				if (first_stats) {
					first_stats = false;
				} else {
					if (fp)
						fmt::fprintf(fp, ",");
				}
				printStats(args, stats, toDoubleSeconds(time_now - time_prev), fp);
			}
			time_prev = time_now;
		}
	}

	if (fp) {
		fmt::fprintf(fp, "],");
	}

	/* print report */
	if (args.verbose >= VERBOSE_DEFAULT) {
		auto time_now = steady_clock::now();
		while (stopcount.load() < args.num_threads * args.num_processes) {
			usleep(10000); /* 10ms */
		}
		printReport(args, stats, toDoubleSeconds(time_now - time_start), pid_main, fp);
	}

	if (fp) {
		fmt::fprintf(fp, "}");
		fclose(fp);
	}

	return 0;
}

int main(int argc, char* argv[]) {
	setlinebuf(stdout);

	auto rc = int{};
	auto args = Arguments{};
	rc = initArguments(args);
	if (rc < 0) {
		logr.error("initArguments failed");
		return -1;
	}
	rc = parseArguments(argc, argv, args);
	if (rc < 0) {
		/* usage printed */
		return 0;
	}

	rc = validateArguments(args);
	if (rc < 0)
		return -1;
	logr.setVerbosity(args.verbose);

	if (args.mode == MODE_CLEAN) {
		/* cleanup will be done from a single thread */
		args.num_processes = 1;
		args.num_threads = 1;
	}

	if (args.mode == MODE_BUILD) {
		if (args.txnspec.ops[OP_INSERT][OP_COUNT] == 0) {
			parseTransaction(args, "i100");
		}
	}

	const auto pid_main = getpid();
	/* create the shared memory for stats */
	const auto shmpath = fmt::format("mako{}", pid_main);
	auto shmfd = shm_open(shmpath.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (shmfd < 0) {
		logr.error("shm_open failed: {}", strerror(errno));
		return -1;
	}
	auto shmfd_guard = ExitGuard([shmfd, &shmpath]() {
		close(shmfd);
		shm_unlink(shmpath.c_str());
		unlink(shmpath.c_str());
	});

	const auto async_mode = args.async_xacts > 0;
	const auto nthreads_for_shm = async_mode ? args.async_xacts : args.num_threads;
	/* allocate */
	const auto shmsize = shared_memory::storageSize(args.num_processes, nthreads_for_shm);

	auto shm = std::add_pointer_t<void>{};
	if (ftruncate(shmfd, shmsize) < 0) {
		shm = MAP_FAILED;
		logr.error("ftruncate (fd:{} size:{}) failed", shmfd, shmsize);
		return -1;
	}

	/* map it */
	shm = mmap(NULL, shmsize, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	if (shm == MAP_FAILED) {
		logr.error("mmap (fd:{} size:{}) failed", shmfd, shmsize);
		return -1;
	}
	auto munmap_guard = ExitGuard([=]() { munmap(shm, shmsize); });

	auto shm_access = shared_memory::Access(shm, args.num_processes, nthreads_for_shm);

	/* initialize the shared memory */
	shm_access.initMemory();

	/* get ready */
	auto& shm_hdr = shm_access.header();
	shm_hdr.signal = SIGNAL_OFF;
	shm_hdr.readycount = 0;
	shm_hdr.stopcount = 0;
	shm_hdr.throttle_factor = 1.0;

	auto proc_type = ProcKind::MAIN;
	/* fork worker processes + 1 stats process */
	auto worker_pids = std::vector<pid_t>(args.num_processes + 1);

	auto worker_id = int{};

	/* forking (num_process + 1) children */
	/* last process is the stats handler */
	for (auto p = 0; p < args.num_processes + 1; p++) {
		auto pid = fork();
		if (pid != 0) {
			/* master */
			worker_pids[p] = pid;
			if (args.verbose == VERBOSE_DEBUG) {
				logr.debug("worker {} (PID:{}) forked", p + 1, worker_pids[p]);
			}
		} else {
			if (p < args.num_processes) {
				/* worker process */
				logr = Logger(WorkerProcess{}, args.verbose, p);
				proc_type = ProcKind::WORKER;
				worker_id = p;
			} else {
				/* stats */
				logr = Logger(StatsProcess{}, args.verbose);
				proc_type = ProcKind::STATS;
			}
			break;
		}
	}

	/* initialize the randomizer */
	srand(time(0) * getpid());

	/* initialize zipfian if necessary (per-process) */
	if (args.zipf) {
		zipfian_generator(args.rows);
	}

	if (proc_type == ProcKind::WORKER) {
		/* worker process */
		workerProcessMain(args, worker_id, shm_access, pid_main);
		/* worker can exit here */
		exit(0);
	} else if (proc_type == ProcKind::STATS) {
		/* stats */
		if (args.mode == MODE_CLEAN) {
			/* no stats needed for clean mode */
			exit(0);
		}
		statsProcessMain(
		    args, shm_access.statsConstArray(), shm_hdr.throttle_factor, shm_hdr.signal, shm_hdr.stopcount, pid_main);
		exit(0);
	}

	/* master */
	/* wait for everyone to be ready */
	while (shm_hdr.readycount.load() < (args.num_processes * args.num_threads)) {
		usleep(1000);
	}
	shm_hdr.signal.store(SIGNAL_GREEN);

	if (args.mode == MODE_RUN) {
		/* run the benchmark */

		/* if seconds is specified, stop child processes after the specified
		 * duration */
		if (args.seconds > 0) {
			logr.debug("master sleeping for {} seconds", args.seconds);

			auto time_start = steady_clock::now();
			while (1) {
				usleep(100000); /* sleep for 100ms */
				auto time_now = steady_clock::now();
				/* doesn't have to be precise */
				if (toDoubleSeconds(time_now - time_start) > args.seconds) {
					logr.debug("time's up ({} seconds)", args.seconds);
					break;
				}
			}

			/* notify everyone the time's up */
			shm_hdr.signal.store(SIGNAL_RED);
		}
	}

	auto status = int{};
	/* wait for worker processes to exit */
	for (auto p = 0; p < args.num_processes; p++) {
		logr.debug("waiting for worker process {} (PID:{}) to exit", p + 1, worker_pids[p]);
		auto pid = waitpid(worker_pids[p], &status, 0 /* or what? */);
		if (pid < 0) {
			logr.error("waitpid failed for worker process PID {}", worker_pids[p]);
		}
		logr.debug("worker {} (PID:{}) exited", p + 1, worker_pids[p]);
	}

	/* all worker threads finished, stop the stats */
	if (args.mode == MODE_BUILD || args.iteration > 0) {
		shm_hdr.signal.store(SIGNAL_RED);
	}

	/* wait for stats to stop */
	auto pid = waitpid(worker_pids[args.num_processes], &status, 0 /* or what? */);
	if (pid < 0) {
		logr.error("waitpid failed for stats process PID {}", worker_pids[args.num_processes]);
	}

	return 0;
}
