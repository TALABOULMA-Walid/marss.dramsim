//
// PTLsim: Cycle Accurate x86-64 Simulator
// Shared Functions and Structures
//
// Copyright 2000-2008 Matt T. Yourst <yourst@yourst.com>
//
// Modifications for MARSSx86
// Copyright 2009 Avadh Patel <avadh4all@gmail.com>
//

#include <globals.h>
#include <ptlsim.h>
#include <memoryStats.h>
#include <elf.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <bson/bson.h>
#include <bson/mongo.h>
#include <machine.h>
#include <statelist.h>
#include <decode.h>

#include <fstream>
#include <syscalls.h>
#include <ptl-qemu.h>

#include <test.h>
/*
 * DEPRECATED CONFIG OPTIONS:
 perfect_cache
 verify_cache (this one implements 'data' in caches, it might be helpful to find bugs)
 */

#ifndef CONFIG_ONLY
//
// Global variables
//
PTLsimConfig config;
ConfigurationParser<PTLsimConfig> configparser;
PTLsimMachine ptl_machine;

ofstream ptl_logfile;
#ifdef TRACE_RIP
ofstream ptl_rip_trace;
#endif
ofstream trace_mem_logfile;
ofstream yaml_stats_file;
bool logenable = 0;
W64 sim_cycle = 0;
W64 unhalted_cycle_count = 0;
W64 iterations = 0;
W64 total_uops_executed = 0;
W64 total_uops_committed = 0;
W64 total_user_insns_committed = 0;
W64 total_basic_blocks_committed = 0;

W64 last_printed_status_at_ticks;
W64 last_printed_status_at_user_insn;
W64 last_printed_status_at_cycle;
W64 ticks_per_update;

W64 last_stats_captured_at_cycle = 0;
W64 tsc_at_start ;

const char *snapshot_names[] = {"user", "kernel", "global"};

Stats *user_stats;
Stats *kernel_stats;
Stats *global_stats;

ofstream *time_stats_file;

#endif

static void kill_simulation();
static void write_mongo_stats();
static void setup_sim_stats();

/* Stats structure for Simulation Statistics */
struct SimStats : public Statable
{
    struct version : public Statable
    {
        StatString git_commit;
        StatString git_branch;
        StatString git_timestamp;
        StatString build_timestamp;
        StatString build_hostname;
        StatString build_compiler;

        version(Statable *parent)
            : Statable("version", parent)
              , git_commit("git_commit", this)
              , git_branch("git_branch", this)
              , git_timestamp("git_timestamp", this)
              , build_timestamp("build_timestamp", this)
              , build_hostname("build_hostname", this)
              , build_compiler("build_compiler", this)
        { }
    } version;

    struct run : public Statable
    {
        StatObj<W64> timestamp;
        StatString   hostname;
        StatObj<W64> native_hz;
        StatObj<W64> seconds;

        run(Statable *parent)
            : Statable("run", parent)
              , timestamp("timestamp", this)
              , hostname("hostname", this)
              , native_hz("native_hz", this)
              , seconds("seconds", this)
        { }
    } run;

    struct performance : public Statable
    {
        StatObj<W64> cycles_per_sec;
        StatObj<W64> commits_per_sec;

        performance(Statable *parent)
            : Statable("performance", parent)
              , cycles_per_sec("cycles_per_sec", this)
              , commits_per_sec("commits_per_sec", this)
        { }
    } performance;

    StatString tags;

    SimStats()
        : Statable("simulator")
          , tags("tags", this)
          , version(this)
          , run(this)
          , performance(this)
    {
        tags.set_split(",");
    }
} simstats;


void PTLsimConfig::reset() {
  help=0;
  run = 0;
  stop = 0;
  kill = 0;
  flush_command_queue = 0;

  quiet = 0;
  core_name = "base"; /* core_name no longer user setable, the machine builder
                         will handle setting this */
  log_filename = "ptlsim.log";
  loglevel = 0;
  start_log_at_iteration = 0;
  start_log_at_rip = INVALIDRIP;
  log_on_console = 0;
  log_buffer_size = 524288;
  log_file_size = 1<<26;
  screenshot_file = "";
  log_user_only = 0;

  dump_state_now = 0;

  verify_cache = 0;
  stats_filename.reset();
  yaml_stats_filename="";
  snapshot_cycles = infinity;
  snapshot_now.reset();
  time_stats_logfile = "";
  time_stats_period = 10000;

  start_at_rip = INVALIDRIP;

  // memory model
  use_memory_model = 0;
  kill_after_run = 0;
  stop_at_user_insns = infinity;
  stop_at_cycle = infinity;
  stop_at_iteration = infinity;
  stop_at_rip = INVALIDRIP;
  stop_at_marker = infinity;
  stop_at_marker_hits = infinity;
  stop_at_user_insns_relative = infinity;
  insns_in_last_basic_block = 65536;
  flush_interval = infinity;
  event_trace_record_filename.reset();
  event_trace_record_stop = 0;
  event_trace_replay_filename.reset();

  core_freq_hz = 0;
  // default timer frequency is 100 hz in time-xen.c:

  perfect_cache = 0;

  dumpcode_filename = "test.dat";
  dump_at_end = 0;
  bbcache_dump_filename.reset();

  machine_config = "";

  ///
  /// memory hierarchy implementation
  ///

  checker_enabled = 0;
  checker_start_rip = INVALIDRIP;

  // MongoDB configuration
  enable_mongo = 0;
  mongo_server = "127.0.0.1";
  mongo_port = 27017;
  bench_name = "";
  tags = "";

  // Test Framework
  run_tests = 0;

  // Utilities/Tools
  execute_after_kill = "";
}

template <>
void ConfigurationParser<PTLsimConfig>::setup() {
  // Full system only
  section("PTLmon Control");
  add(help,                       "help",               "Print this message");

  section("Action (specify only one)");
  add(run,                          "run",                  "Run under simulation");
  add(stop,                         "stop",                 "Stop current simulation run and wait for command");
  add(kill,                         "kill",                 "Kill PTLsim inside domain (and ptlmon), then shutdown domain");
  add(flush_command_queue,          "flush",                "Flush all queued commands, stop the current simulation run and wait");

  section("General Logging Control");
  add(quiet,                        "quiet",                "Do not print PTLsim system information banner");
  add(log_filename,                 "logfile",              "Log filename (use /dev/fd/1 for stdout, /dev/fd/2 for stderr)");
  add(loglevel,                     "loglevel",             "Log level (0 to 99)");
  add(start_log_at_iteration,       "startlog",             "Start logging after iteration <startlog>");
  add(start_log_at_rip,             "startlogrip",          "Start logging after first translation of basic block starting at rip");
  add(log_on_console,               "consolelog",           "Replicate log file messages to console");
  add(log_buffer_size,              "logbufsize",           "Size of PTLsim ptl_logfile buffer (not related to -ringbuf)");
  add(log_file_size,                "logfilesize",           "Size of PTLsim ptl_logfile");
  add(dump_state_now,               "dump-state-now",       "Dump the event log ring buffer and internal state of the active core");
  add(screenshot_file,              "screenshot",           "Takes screenshot of VM window at the end of simulation");
  add(log_user_only,                "log-user-only",        "Only log the user mode activities");

  section("Statistics Database");
  add(stats_filename,               "stats",                "Statistics data store hierarchy root");
  add(yaml_stats_filename,          "yamlstats",                "Statistics data stores in YAML format");
  add(snapshot_cycles,              "snapshot-cycles",      "Take statistical snapshot and reset every <snapshot> cycles");
  add(snapshot_now,                 "snapshot-now",         "Take statistical snapshot immediately, using specified name");
  add(time_stats_logfile,           "time-stats-logfile",   "File to write time-series statistics (new)");
  add(time_stats_period,            "time-stats-period",    "Frequency of capturing time-stats (in cycles)");
  section("Trace Start/Stop Point");
  add(start_at_rip,                 "startrip",             "Start at rip <startrip>");
  add(stop_at_user_insns,           "stopinsns",            "Stop after executing <stopinsns> user instructions");
  add(stop_at_cycle,                "stopcycle",            "Stop after <stop> cycles");
  add(stop_at_iteration,            "stopiter",             "Stop after <stop> iterations (does not apply to cycle-accurate cores)");
  add(stop_at_rip,                  "stoprip",              "Stop before rip <stoprip> is translated for the first time");
  add(stop_at_marker,               "stop-at-marker",       "Stop after PTLCALL_MARKER with marker X");
  add(stop_at_marker_hits,          "stop-at-marker-hits",  "Stop after PTLCALL_MARKER is called N times");
  add(stop_at_user_insns_relative,  "stopinsns-rel",        "Stop after executing <stopinsns-rel> user instructions relative to start of current run");
  add(insns_in_last_basic_block,    "bbinsns",              "In final basic block, only translate <bbinsns> user instructions");
  add(flush_interval,               "flushevery",           "Flush the pipeline every N committed instructions");
  add(kill_after_run,               "kill-after-run",       "Kill PTLsim after this run");
  section("Event Trace Recording");
  add(event_trace_record_filename,  "event-record",         "Save replayable events (interrupts, DMAs, etc) to this file");
  add(event_trace_record_stop,      "event-record-stop",    "Stop recording events");
  add(event_trace_replay_filename,  "event-replay",         "Replay events (interrupts, DMAs, etc) to this file, starting at checkpoint");

  section("Timers and Interrupts");
  add(core_freq_hz,                 "corefreq",             "Core clock frequency in Hz (default uses host system frequency)");

  section("Validation");
  add(checker_enabled, 		"enable-checker", 		"Enable emulation based checker");
  add(checker_start_rip,          "checker-startrip",     "Start checker at specified RIP");

  section("Out of Order Core (ooocore)");
  add(perfect_cache,                "perfect-cache",        "Perfect cache performance: all loads and stores hit in L1");

  section("Miscellaneous");
  add(dumpcode_filename,            "dumpcode",             "Save page of user code at final rip to file <dumpcode>");
  add(dump_at_end,                  "dump-at-end",          "Set breakpoint and dump core before first instruction executed on return to native mode");
  add(bbcache_dump_filename,        "bbdump",               "Basic block cache dump filename");

 add(verify_cache,               "verify-cache",                   "run simulation with storing actual data in cache");

  section("Core Configuration");
  stringbuf* m_names = new stringbuf();
  *m_names << "Available Machine: ";
  MachineBuilder::get_all_machine_names(*m_names);
  add(machine_config, "machine", m_names->buf);

 ///
 /// following are for the new memory hierarchy implementation:
 ///

  section("Memory Hierarchy Configuration");
  //  add(memory_log,               "memory-log",               "log memory debugging info");

  // MongoDB
  section("bus configuration");
  add(enable_mongo,         "enable-mongo",         "Enable storing data to MongoDB serve");
  add(mongo_server,         "mongo-server",         "Server Address running MongoDB");
  add(mongo_port,           "mongo-port",           "MongoDB server's port address");
  add(bench_name,           "bench-name",           "Benchmark Name added to database");
  add(tags,              "tags",              "tags added to database");

  // Test Framework
  section("Unit Test Framework");
  add(run_tests,            "run-tests",            "Run Test cases");

  // Utilities/Tools
  section("options for tools/utilities");
  add(execute_after_kill,	"execute-after-kill" ,	"Execute a shell command (on the host shell) after simulation receives kill signal");
};

#ifndef CONFIG_ONLY

ostream& operator <<(ostream& os, const PTLsimConfig& config) {
  return configparser.print(os, config);
}

static void print_banner(ostream& os) {
  utsname hostinfo;
  sys_uname(&hostinfo);

  os << "//  ", endl;
  os << "//  MARSS: Cycle Accurate Systems simulator for x86", endl;
  os << "//  Copyright 1999-2007 Matt T. Yourst <yourst@yourst.com>", endl;
  os << "//  Copyright 2009-2011 Avadh Patel <avadh4all@gmail.com>", endl;
  os << "// ", endl;
  os << "//  Git branch '", stringify(GITBRANCH), "' on date ", stringify(GITDATE)," (HEAD: ", stringify(GITCOMMIT), ")", endl;
  os << "//  Built ", __DATE__, " ", __TIME__, " on ", stringify(BUILDHOST), " using gcc-",
    stringify(__GNUC__), ".", stringify(__GNUC_MINOR__), endl;
  os << "//  Running on ", hostinfo.nodename, ".", hostinfo.domainname, endl;
  os << "//  ", endl;
  os << endl;
  os << flush;
}

static void collect_common_sysinfo() {
  utsname hostinfo;
  sys_uname(&hostinfo);

  stringbuf sb;
  sb.reset(); sb << __DATE__, " ", __TIME__;
  simstats.version.build_timestamp = sb;
  simstats.version.build_hostname = stringify(BUILDHOST);
  sb.reset(); sb << "gcc-", __GNUC__, ".", __GNUC_MINOR__;
  simstats.version.build_compiler = sb;
  simstats.version.git_branch = stringify(GITBRANCH);
  simstats.version.git_commit = stringify(GITCOMMIT);
  simstats.version.git_timestamp = stringify(GITDATE);

  W64 time = sys_time(0);
  simstats.run.timestamp = time;
  sb.reset(); sb << hostinfo.nodename, ".", hostinfo.domainname;
  simstats.run.hostname = sb;
  W64 hz = get_core_freq_hz();
  simstats.run.native_hz = hz;
}

void print_usage() {
  cerr << "Syntax: simulate <arguments...>", endl;
  cerr << "In the monitor mode give the above command with options given below", endl, endl;

  configparser.printusage(cerr, config);
}

stringbuf current_stats_filename;
stringbuf current_log_filename;
stringbuf current_bbcache_dump_filename;
stringbuf current_trace_memory_updates_logfile;
stringbuf current_yaml_stats_filename;
W64 current_start_sim_rip;

void backup_and_reopen_logfile() {
  if (config.log_filename) {
    if (ptl_logfile) ptl_logfile.close();
    stringbuf oldname;
    oldname << config.log_filename, ".backup";
    sys_unlink(oldname);
    sys_rename(config.log_filename, oldname);
    ptl_logfile.open(config.log_filename);
  }
}

void backup_and_reopen_yamlstats() {
  if (config.yaml_stats_filename) {
    if (yaml_stats_file) yaml_stats_file.close();
    stringbuf oldname;
    oldname << config.yaml_stats_filename, ".backup";
    sys_unlink(oldname);
    sys_rename(config.yaml_stats_filename, oldname);
    yaml_stats_file.open(config.yaml_stats_filename);
  }
}

void force_logging_enabled() {
  logenable = 1;
  config.start_log_at_iteration = 0;
  config.loglevel = 99;
}

extern byte _binary_ptlsim_build_ptlsim_dst_start;
extern byte _binary_ptlsim_build_ptlsim_dst_end;

void capture_stats_snapshot(const char* name) {
  if (logable(100)|1) {
    if (name) ptl_logfile << "Snapshot named ", name;
    ptl_logfile << " at cycle ", sim_cycle, endl;
  }

  /* TODO: Support stats snapshot in new Stats module */
}

void print_sysinfo(ostream& os) {
	// TODO: In QEMU based system
}

void dump_yaml_stats()
{
    if(!config.yaml_stats_filename) {
        return;
    }

    YAML::Emitter k_out, u_out, g_out;

    (StatsBuilder::get()).dump(kernel_stats, k_out);
    yaml_stats_file << k_out.c_str() << "\n";

    (StatsBuilder::get()).dump(user_stats, u_out);
    yaml_stats_file << u_out.c_str() << "\n";

    (StatsBuilder::get()).dump(global_stats, g_out);
    yaml_stats_file << g_out.c_str() << "\n";

    yaml_stats_file.flush();
}

static void flush_stats()
{
    if(config.screenshot_file.buf != "") {
        qemu_take_screenshot((char*)config.screenshot_file);
    }

    PTLsimMachine* machine = PTLsimMachine::getmachine(config.core_name.buf);
    assert(machine);
    machine->update_stats();

    // Call this function to setup tags and other info
    setup_sim_stats();

    dump_yaml_stats();

    if(config.enable_mongo)
        write_mongo_stats();

    if(time_stats_file) {
        time_stats_file->close();
    }
}

static void kill_simulation()
{
    assert(config.kill || config.kill_after_run);

    ptl_logfile << "Received simulation kill signal, stopped the simulation and killing the VM\n";
#ifdef TRACE_RIP
    ptl_rip_trace.flush();
    ptl_rip_trace.close();
#endif

    if (config.execute_after_kill.size() > 0) {
        ptl_logfile << "Executing: " << config.execute_after_kill << endl;
        int ret = system(config.execute_after_kill.buf);
        if(ret != 0) {
            ptl_logfile << "execute-after-kill command return " << ret << endl;
        }
    }

    ptl_logfile.flush();
    ptl_logfile.close();

    ptl_quit();
}

bool handle_config_change(PTLsimConfig& config, int argc, char** argv) {
  static bool first_time = true;

  if (config.log_filename.set() && (config.log_filename != current_log_filename)) {
    // Can also use "-ptl_logfile /dev/fd/1" to send to stdout (or /dev/fd/2 for stderr):
    backup_and_reopen_logfile();
    current_log_filename = config.log_filename;
  }
#ifdef TRACE_RIP
	if(!ptl_rip_trace.is_open())
		ptl_rip_trace.open("ptl_rip_trace");
#endif

    if(config.stats_filename.set() && (config.stats_filename != current_yaml_stats_filename)) {
        config.yaml_stats_filename = config.stats_filename;
        backup_and_reopen_yamlstats();
        current_yaml_stats_filename = config.stats_filename;
    }

  if (config.yaml_stats_filename.set() &&
          (config.yaml_stats_filename != current_yaml_stats_filename) &&
          !config.stats_filename.set()) {
    backup_and_reopen_yamlstats();
    current_yaml_stats_filename = config.yaml_stats_filename;
  }

if ((config.loglevel > 0) & (config.start_log_at_rip == INVALIDRIP) & (config.start_log_at_iteration == infinity)) {
    config.start_log_at_iteration = 0;
  }

  //
  // Fix up parameter defaults:
  //
  if (config.start_log_at_rip != INVALIDRIP) {
    config.start_log_at_iteration = infinity;
    logenable = 0;
  } else if (config.start_log_at_iteration != infinity) {
    config.start_log_at_rip = INVALIDRIP;
    logenable = 0;
  }

  if (config.bbcache_dump_filename.set() && (config.bbcache_dump_filename != current_bbcache_dump_filename)) {
    // Can also use "-ptl_logfile /dev/fd/1" to send to stdout (or /dev/fd/2 for stderr):
    bbcache_dump_file.open(config.bbcache_dump_filename);
    current_bbcache_dump_filename = config.bbcache_dump_filename;
  }

#ifdef __x86_64__
  config.start_log_at_rip = signext64(config.start_log_at_rip, 48);
  config.start_at_rip = signext64(config.start_at_rip, 48);
  config.stop_at_rip = signext64(config.stop_at_rip, 48);
#endif

  if(config.run && !config.kill && !config.stop) {
	  start_simulation = 1;
  }

  if(config.start_at_rip != INVALIDRIP && current_start_sim_rip != config.start_at_rip) {
    ptl_start_sim_rip = config.start_at_rip;
    current_start_sim_rip = config.start_at_rip;
  }

  if((start_simulation || in_simulation) && config.stop) {
      if(config.run)
          config.run = false;
  }

  if(config.kill) {
	  config.run = false;
  }

  if (first_time) {
    if (!config.quiet) {
      print_sysinfo(cerr);
      if (!(config.run | config.kill))
        cerr << "Simulator is now waiting for a 'run' command.", endl, flush;
    }
    print_banner(ptl_logfile);
    print_sysinfo(ptl_logfile);
    cerr << flush;
    ptl_logfile << config;
    ptl_logfile.flush();
    first_time = false;
  }

  int total = config.run + config.stop + config.kill;
  if (total > 1) {
    ptl_logfile << "Warning: only one action (from -run, -stop, -kill) can be specified at once", endl, flush;
    cerr << "Warning: only one action (from -run, -stop, -kill) can be specified at once", endl, flush;
  }

  if(config.checker_enabled) {
    if(config.checker_start_rip == INVALIDRIP)
        enable_checker();
    else
        config.checker_enabled = false;
  }

  return true;
}

Hashtable<const char*, PTLsimMachine*, 1>* machinetable = NULL;

bool PTLsimMachine::init(PTLsimConfig& config) { return false; }
int PTLsimMachine::run(PTLsimConfig& config) { return 0; }
void PTLsimMachine::update_stats() { return; }
void PTLsimMachine::dump_state(ostream& os) { return; }
void PTLsimMachine::flush_tlb(Context& ctx) { return; }
void PTLsimMachine::flush_tlb_virt(Context& ctx, Waddr virtaddr) { return; }

void PTLsimMachine::addmachine(const char* name, PTLsimMachine* machine) {
  if unlikely (!machinetable) {
    machinetable = new Hashtable<const char*, PTLsimMachine*, 1>();
  }
  machinetable->add(name, machine);
  machine->first_run = 0;
}
void PTLsimMachine::removemachine(const char* name, PTLsimMachine* machine) {
  machinetable->remove(name, machine);
}

PTLsimMachine* PTLsimMachine::getmachine(const char* name) {
  if unlikely (!machinetable) return NULL;
  PTLsimMachine** p = machinetable->get(name);
  if (!p) return NULL;
  return *p;
}

/* Currently executing machine model: */
PTLsimMachine* curr_ptl_machine = NULL;

PTLsimMachine* PTLsimMachine::getcurrent() {
  return curr_ptl_machine;
}

void ptl_reconfigure(char* config_str) {

	char* argv[1]; argv[0] = config_str;

	if(config_str == NULL || strlen(config_str) == 0) {
		print_usage();
		return;
	}

	configparser.parse(config, config_str);
	handle_config_change(config, 1, argv);
	ptl_logfile << "Configuration changed: ", config, endl;

    /*
	 * set the curr_ptl_machine to NULL so it will be automatically changed to
	 * new configured machine
     */
	curr_ptl_machine = NULL;
}

extern "C" void ptl_machine_configure(const char* config_str_) {

    static bool ptl_machine_configured=false;

    char *config_str = (char*)qemu_mallocz(strlen(config_str_) + 1);
    pstrcpy(config_str, strlen(config_str_)+1, config_str_);

    if(!ptl_machine_configured) {
        configparser.setup();
        config.reset();
    }

    // Setup the configuration
    ptl_reconfigure(config_str);

    // After reconfigure reset the machine's initalized variable
    if (config.help){
        configparser.printusage(cerr, config);
        config.help=0;
    }

    if(config.kill) {
        flush_stats();
        kill_simulation();
    }

    // reset machine's initalized variable only if it is the first run


    if(!ptl_machine_configured){
        ptl_machine_configured=true;
        PTLsimMachine* machine = NULL;
        char* machinename = config.core_name;
        if likely (curr_ptl_machine != NULL) {
            machine = curr_ptl_machine;
        } else {
            machine = PTLsimMachine::getmachine(machinename);
        }
        assert(machine);
        machine->initialized = 0;

        // Setup YAML Stats
        StatsBuilder& builder = StatsBuilder::get();
        user_stats = builder.get_new_stats();
        kernel_stats = builder.get_new_stats();
        global_stats = builder.get_new_stats();

        // time based stats
        if (config.time_stats_logfile.length > 0)
        {
            time_stats_file = new ofstream(config.time_stats_logfile.buf);
            builder.init_timer_stats();
        } else {
            time_stats_file = NULL;
        }
    }

    qemu_free(config_str);

    ptl_machine.disable_dump();

    if(config.run_tests) {
        in_simulation = 1;
    }
}

extern "C"
CPUX86State* ptl_create_new_context() {

    static int ctx_counter = 0;
	assert(ctx_counter < contextcount);

	// Create a new CPU context and add it to contexts array
	Context* ctx = new Context();
	ptl_contexts[ctx_counter] = ctx;
	ctx_counter++;

	return (CPUX86State*)(ctx);
}

/* Checker */
Context* checker_context = NULL;

void enable_checker() {

    if(checker_context != NULL) {
        delete checker_context;
    }

    checker_context = new Context();
    memset(checker_context, 0, sizeof(Context));
}

void setup_checker(W8 contextid) {

    /* First clear the old check_context */
    assert(checker_context);

    checker_context->setup_ptlsim_switch();

    if(checker_context->kernel_mode || checker_context->eip == 0) {
      in_simulation = 0;
      tb_flush(ptl_contexts[0]);
      in_simulation = 1;
      memset(checker_context, 0, sizeof(Context));

      /* Copy the context of given contextid */
      memcpy(checker_context, ptl_contexts[contextid], sizeof(Context));

      if(logable(10)) {
	ptl_logfile << "Checker context setup\n", *checker_context, endl;
      }
    }

    if(logable(10)) {
      ptl_logfile << "No change to checker context ", checker_context->kernel_mode, endl;
    }
}

void clear_checker() {
    assert(checker_context);
    memset(checker_context, 0, sizeof(Context));
}

bool is_checker_valid() {
    return (checker_context->eip == 0 ? false : true);
}

void execute_checker() {

    /* Fist make sure that checker context is in User mode */
    // TODO : Enable kernel mode checker
    assert(checker_context->kernel_mode == 0);

    /* Set the checker_context as global env */
    checker_context->setup_qemu_switch();

    /* We need to load eflag's condition flags manually */
    // load_eflags(checker_context->reg_flags, FLAG_ZAPS|FLAG_CF|FLAG_OF);

    checker_context->singlestep_enabled = SSTEP_ENABLE;

    in_simulation = 0;

    checker_context->interrupt_request = 0;
    checker_context->handle_interrupt = 0;
    checker_context->exception_index = 0;
    W64 old_eip = checker_context->eip;
    int old_exception_index = checker_context->exception_index;
    int ret;
    while(checker_context->eip == old_eip)
        ret = cpu_exec((CPUX86State*)checker_context);

    checker_context->exception_index = old_exception_index;

    checker_context->setup_ptlsim_switch();

    if(checker_context->interrupt_request != 0)
        ptl_contexts[0]->interrupt_request = checker_context->interrupt_request;

    if(checker_context->kernel_mode) {
      // TODO : currently we skip the context switch from checker
      // we 0 out the checker_context so it will setup when next time
      // some one calls setup_checker
      memset(checker_context, 0, sizeof(Context));
    }

    in_simulation = 1;

    if(logable(4)) {
        ptl_logfile << "Checker execution ret value: ", ret, endl;
        ptl_logfile << "Checker flags: ", (void*)checker_context->eflags, endl;
    }
}

void compare_checker(W8 context_id, W64 flagmask) {
    int ret, ret1, ret_x87;
    int check_size = (char*)(&(checker_context->eip)) - (char*)(checker_context);
    //ptl_logfile << "check_size: ", check_size, endl;

    if(checker_context->eip == 0) {
      return;
    }

    ret = memcmp(checker_context, ptl_contexts[context_id], check_size);

    check_size = (sizeof(XMMReg) * 16);
    //ptl_logfile << "check_size: ", check_size, endl;

    ret1 = memcmp(&checker_context->xmm_regs, &ptl_contexts[context_id]->xmm_regs, check_size);

    check_size = (sizeof(FPReg) * 8);
    ret_x87 = memcmp(&checker_context->fpregs, &ptl_contexts[context_id]->fpregs, check_size);
    ret_x87 = 0;

    bool fail = false;
    fail = (checker_context->eip != ptl_contexts[context_id]->eip);

    W64 flag1 = checker_context->reg_flags & flagmask & ~(FLAG_INV | FLAG_AF | FLAG_PF);
    W64 flag2 = ptl_contexts[context_id]->reg_flags & flagmask & ~(FLAG_INV | FLAG_AF | FLAG_PF);
    //fail |= (flag1 != flag2);

    if(ret != 0 || ret1 != 0 || ret_x87 != 0 || fail) {
        ptl_logfile << "Checker comparison failed [diff-chars: ", ret, "] ";
        ptl_logfile << "[xmm:", ret1, "] [x87:", ret_x87,"] ";
        ptl_logfile << "[flags:", fail, "]\n";
        ptl_logfile << "CPU Context:\n", *ptl_contexts[context_id], endl;
        ptl_logfile << "Checker Context:\n", *checker_context, endl, flush;

        cout << "\n*******************Failed checker***************\n";
        memset(checker_context, 0, sizeof(Context));
        // assert(0);
    }
}

void setup_qemu_switch_all_ctx(Context& last_ctx) {
	foreach(c, contextcount) {
		Context& ctx = contextof(c);
		if(&ctx != &last_ctx)
			ctx.setup_qemu_switch();
	}

	/* last_ctx must setup after all other ctx are set */
	last_ctx.setup_qemu_switch();
}

void setup_qemu_switch_except_ctx(const Context& const_ctx) {
	foreach(c, contextcount) {
		Context& ctx = contextof(c);
		if(&ctx != &const_ctx)
			ctx.setup_qemu_switch();
	}
}

void setup_ptlsim_switch_all_ctx(Context& last_ctx) {
	foreach(c, contextcount) {
		Context& ctx = contextof(c);
		if(&ctx != &last_ctx)
			ctx.setup_ptlsim_switch();
	}

	/* last_ctx must setup after all other ctx are set */
	last_ctx.setup_ptlsim_switch();
}

/* This function is auto-generated by dstbuild_bson.py script at compile time */
void add_bson_PTLsimStats(PTLsimStats *stats, bson_buffer *bb, const char *snapshot_name);

/* Write all the stats to MongoDB */
void write_mongo_stats() {
    bson *bout;
    bson_buffer *bb;
    char numstr[4];
    mongo_connection conn[1];
    mongo_connection_options opts;
    const char *ns = "marss.benchmarks";

    /* First setup the connection to MongoDB */
    strncpy(opts.host, config.mongo_server.buf , 255);
    opts.host[254] = '\0';
    opts.port = config.mongo_port;

    if(mongo_connect(conn, &opts)){
        cerr << "Failed to connect to MongoDB server at ", opts.host,
             ":", opts.port, " , **Skipping Mongo Datawrite**", endl;
        ptl_logfile << "Failed to connect to MongoDB server at ", opts.host,
             ":", opts.port, " , **Skipping Mongo Datawrite**", endl;
        config.enable_mongo = 0;
        return;
    }

    /* Now write user, kernel and global stats into database */
    foreach(i, 3) {
        Stats *stats_;

        bb = (bson_buffer*)qemu_mallocz(sizeof(bson_buffer));
        bout = (bson*)qemu_mallocz(sizeof(bson));

        bson_buffer_init(bb);
        bson_append_new_oid(bb, "_id");

        switch(i) {
            case 0: stats_ = user_stats; break;
            case 1: stats_ = kernel_stats; break;
            case 2: stats_ = global_stats; break;
        }

        bb = (StatsBuilder::get()).dump(stats_, bb);
        bson_from_buffer(bout, bb);

        mongo_insert(conn, ns, bout);
        bson_destroy(bout);

        qemu_free(bb);
        qemu_free(bout);
    }

    /* Close the connection with MongoDB */
    mongo_destroy(conn);
}

stringbuf get_date()
{
    time_t rawtime;
    struct tm *timeinfo;
    char buf[80];
    stringbuf date;

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buf, 80, "%F", timeinfo);
    date << buf;

    return date;
}

static void set_run_stats()
{
    static W64 seconds = 0;
    W64 tsc_at_end = rdtsc();
    seconds += W64(ticks_to_seconds(tsc_at_end - tsc_at_start));
    W64 cycles_per_sec = W64(double(sim_cycle) / double(seconds));
    W64 commits_per_sec = W64(
            double(total_user_insns_committed) / double(seconds));

#define RUN_STAT(stat) \
    simstats.set_default_stats(stat); \
    simstats.run.seconds = seconds; \
    simstats.performance.cycles_per_sec = cycles_per_sec; \
    simstats.performance.commits_per_sec = commits_per_sec;

    RUN_STAT(user_stats);
    RUN_STAT(kernel_stats);
    RUN_STAT(global_stats);
#undef RUN_STAT
}

static void setup_sim_stats()
{
    set_run_stats();

    /* Simlation tags contains benchmark name, host name, simulation-date,
     * user specified tags */
    stringbuf base_tags, kernel_tags, user_tags, total_tags;
    utsname hostinfo;
    stringbuf date;

    sys_uname(&hostinfo);
    date = get_date();

    base_tags << config.machine_config << ",";

    if(config.bench_name.size() > 0)
        base_tags << config.bench_name << ",";
    base_tags << hostinfo.nodename << "." << hostinfo.domainname << ",";
    base_tags << date << ",";

    if(config.tags.size() > 0)
        base_tags << config.tags << ",";

    kernel_tags << base_tags << "kernel";
    user_tags << base_tags << "user";
    total_tags << base_tags << "total";
    ptl_logfile << "Total Tags: " << total_tags << endl;

    simstats.tags.set(kernel_stats, kernel_tags);
    simstats.tags.set(user_stats, user_tags);
    simstats.tags.set(global_stats, total_tags);

#define COLLECT_SYSINFO(stat) \
    simstats.set_default_stats(stat); \
    collect_common_sysinfo();

    COLLECT_SYSINFO(user_stats);
    COLLECT_SYSINFO(kernel_stats);
    COLLECT_SYSINFO(global_stats);
#undef COLLECT_SYSINFO
}

extern "C" uint8_t ptl_simulate() {
	PTLsimMachine* machine = NULL;
	char* machinename = config.core_name;
	if likely (curr_ptl_machine != NULL) {
		machine = curr_ptl_machine;
	} else {
		machine = PTLsimMachine::getmachine(machinename);
	}

	if (!machine) {
		ptl_logfile << "Cannot find core named '", machinename, "'", endl;
		cerr << "Cannot find core named '", machinename, "'", endl;
		return 0;
	}

    // If config.run_tests is enabled, then run testcases
    if(config.run_tests) {
        run_tests();
    }

	if (!machine->initialized) {
		ptl_logfile << "Initializing core '", machinename, "'", endl;
		if (!machine->init(config)) {
			ptl_logfile << "Cannot initialize core model; check its configuration!", endl;
			return 0;
		}
		machine->initialized = 1;
		machine->first_run = 1;

		if(logable(1)) {
			ptl_logfile << "Switching to simulation core '", machinename, "'...", endl, flush;
			cerr <<  "Switching to simulation core '", machinename, "'...", endl, flush;
			ptl_logfile << "Stopping after ", config.stop_at_user_insns, " commits", endl, flush;
			cerr << "Stopping after ", config.stop_at_user_insns, " commits", endl, flush;
		}

		/* Update stats every half second: */
		ticks_per_update = seconds_to_ticks(0.2);
		last_printed_status_at_ticks = 0;
		last_printed_status_at_user_insn = 0;
		last_printed_status_at_cycle = 0;

		tsc_at_start = rdtsc();
		curr_ptl_machine = machine;

        if(config.enable_mongo) {
            // Check MongoDB connection
            hostent *host;
            mongo_connection conn[1];
            mongo_connection_options opts;

            host = gethostbyname(config.mongo_server.buf);
            if(host == NULL) {
                cerr << "MongoDB Server host ", config.mongo_server, " is unreachable.", endl;
                config.enable_mongo = 0;
            } else {
                config.mongo_server = inet_ntoa(*((in_addr *)host->h_addr));
                strncpy(opts.host, config.mongo_server.buf , 255);
                opts.host[254] = '\0';
                opts.port = config.mongo_port;

                if (mongo_connect( conn , &opts )){
                    cerr << "Failed to connect to MongoDB server at ", opts.host,
                         ":", opts.port, " , **Disabling Mongo Support**", endl;
                    config.enable_mongo = 0;
                } else {
                    mongo_destroy(conn);
                }
            }
        }
	}

	foreach(ctx_no, contextcount) {
		Context& ctx = contextof(ctx_no);
		ctx.setup_ptlsim_switch();
		ctx.running = 1;
	}

	ptl_logfile << flush;
    if(ptl_stable_state == 0) {
        machine->dump_state(ptl_logfile);
        assert_fail(__STRING(ptl_stable_state == 0), __FILE__,
            __LINE__, __PRETTY_FUNCTION__);
    }

    /*
	 * Set ret_qemu_env to NULL, it will be set at the exit of simulation 'run'
	 * to the Context that has interrupts/exceptions pending
     */
	machine->ret_qemu_env = NULL;
	ptl_stable_state = 0;

	if(machine->stopped != 0)
		machine->stopped = 0;

    if(logable(1)) {
		ptl_logfile << "Starting simulation at rip: ";
		foreach(i, contextcount) {
			ptl_logfile  << "[cpu ", i, "]", (void*)(contextof(i).eip), " ";
		}
        ptl_logfile << " sim_cycle: ", sim_cycle;
		ptl_logfile << endl;
    }

	machine->run(config);

	if (config.stop_at_user_insns <= total_user_insns_committed || config.kill == true
			|| config.stop == true || config.stop_at_cycle < sim_cycle) {
		machine->stopped = 1;
	}

	ptl_stable_state = 1;

    if(machine->ret_qemu_env)
        setup_qemu_switch_all_ctx(*machine->ret_qemu_env);

	if (!machine->stopped) {
        if(logable(1)) {
			ptl_logfile << "Switching back to qemu rip: ", (void *)contextof(0).get_cs_eip(), " exception: ", contextof(0).exception_index,
						" ex: ", contextof(0).exception, " running: ",
						contextof(0).running;
            ptl_logfile << " sim_cycle: ", sim_cycle;
            ptl_logfile << endl, flush;
        }

		/* Tell QEMU that we will come back to simulate */
		return 1;
	}


	W64 tsc_at_end = rdtsc();
	curr_ptl_machine = NULL;

	W64 seconds = W64(ticks_to_seconds(tsc_at_end - tsc_at_start));
	stringbuf sb;
	sb << endl, "Stopped after ", sim_cycle, " cycles, ", total_user_insns_committed, " instructions and ",
	   seconds, " seconds of sim time (cycle/sec: ", W64(double(sim_cycle) / double(seconds)), " Hz, insns/sec: ", W64(double(total_user_insns_committed) / double(seconds)), ", insns/cyc: ",  double(total_user_insns_committed) / double(sim_cycle), ")", endl;

	ptl_logfile << sb, flush;
	cerr << sb, flush;

	if (config.dumpcode_filename.set()) {
		//    byte insnbuf[256];
		//    PageFaultErrorCode pfec;
		//    Waddr faultaddr;
		//    Waddr rip = contextof(0).eip;
		//    int n = contextof(0).copy_from_user(insnbuf, rip, sizeof(insnbuf), pfec, faultaddr);
		//    ptl_logfile << "Saving ", n, " bytes from rip ", (void*)rip, " to ", config.dumpcode_filename, endl, flush;
		//    ostream(config.dumpcode_filename).write(insnbuf, n);
	}

	last_printed_status_at_ticks = 0;
	cerr << endl;

    flush_stats();

	if(config.kill || config.kill_after_run) {
        kill_simulation();
	}

    machine->first_run = 1;

    if(config.stop) {
        config.stop = false;
    }

    foreach(ctx_no, contextcount) {
        Context& ctx = contextof(ctx_no);
        tb_flush((CPUX86State*)(&ctx));
        ctx.old_eip = 0;
    }

	return 0;
}

extern "C" void update_progress() {
  W64 ticks = rdtsc();
  W64s delta = (ticks - last_printed_status_at_ticks);
  if unlikely (delta < 0) delta = 0;
  if unlikely (delta >= ticks_per_update) {
    double seconds = ticks_to_seconds(delta);
    double cycles_per_sec = (sim_cycle - last_printed_status_at_cycle) / seconds;
    double insns_per_sec = (total_user_insns_committed - last_printed_status_at_user_insn) / seconds;

    stringbuf sb;
    sb << "Completed ", intstring(sim_cycle, 13), " cycles, ", intstring(total_user_insns_committed, 13), " commits: ",
      intstring((W64)cycles_per_sec, 9), " Hz, ", intstring((W64)insns_per_sec, 9), " insns/sec";

    sb << ": rip";
    foreach (i, contextcount) {
      Context& ctx = contextof(i);
      if (!ctx.running) {

		  static const char* runstate_names[] = {"stopped", "running"};
		  const char* runstate_name = runstate_names[ctx.running];

		  sb << " (", runstate_name, ":",ctx.running, ")";
		  if(!sim_cycle){
			  ctx.running = 1;
		  }
		  continue;
      }
      sb << ' ', hexstring(contextof(i).get_cs_eip(), 64);
    }

    //while (sb.size() < 160) sb << ' ';

    ptl_logfile << sb, endl;
    if (!config.quiet) {
        cerr << "\r  ", sb;
    }

    last_printed_status_at_ticks = ticks;
    last_printed_status_at_cycle = sim_cycle;
    last_printed_status_at_user_insn = total_user_insns_committed;
  }

  if unlikely ((sim_cycle - last_stats_captured_at_cycle) >= config.snapshot_cycles) {
    last_stats_captured_at_cycle = sim_cycle;
    capture_stats_snapshot();
  }

  if unlikely (config.snapshot_now.set()) {
    capture_stats_snapshot(config.snapshot_now);
    config.snapshot_now.reset();
  }

}

void dump_all_info() {
	if(curr_ptl_machine) {
		curr_ptl_machine->dump_state(ptl_logfile);
		ptl_logfile.flush();
	}
}

/* IO Signal Support */

struct QemuIOSignal : public FixStateListObject
{
    QemuIOCB fn;
    void *arg;
    W64 cycle;

    void init()
    {
        fn = 0;
        arg = 0;
        cycle = 0;
    }

    void setup(QemuIOCB fn, void *arg, int delay)
    {
        this->fn = fn;
        this->arg = arg;
        this->cycle = sim_cycle + delay;
    }
};

static FixStateList<QemuIOSignal, 32> *qemuIOEvents = NULL;

void init_qemu_io_events()
{
    qemuIOEvents = new FixStateList<QemuIOSignal, 32>();
}

void clock_qemu_io_events()
{
    QemuIOSignal *signal;
    foreach_list_mutable(qemuIOEvents->list(), signal, entry, prev) {
        if (signal->cycle <= sim_cycle) {
            ptl_logfile << "Executing QEMU IO Event at " << sim_cycle << endl;
            signal->fn(signal->arg);
            qemuIOEvents->free(signal);
        }
    }
}

extern "C" void add_qemu_io_event(QemuIOCB fn, void *arg, int delay)
{
    QemuIOSignal* signal = qemuIOEvents->alloc();
    assert(signal);

    signal->setup(fn, arg, delay);

    ptl_logfile << "Added QEMU IO event for " << (sim_cycle + delay) << endl;
}

#endif // CONFIG_ONLY
