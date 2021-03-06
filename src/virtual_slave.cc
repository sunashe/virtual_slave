/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#define MYSQL_CLIENT
#undef MYSQL_SERVER
#include "my_default.h"
#include "my_time.h"
#include "semisync_slave_plugin.h"
#include <string>
using std::string;
#include "mysql.h"
#include "sql_common.h"
#include <mysql/errmsg.h>
/* That one is necessary for defines of OPTION_NO_FOREIGN_KEY_CHECKS etc */
#include "query_options.h"
#include <signal.h>
#include "my_dir.h"

#include "prealloced_array.h"
#include "virtual_slave.h"
#include "Config/Config.h"
#include "log/vs_log.h"

/*
  error() is used in macro BINLOG_ERROR which is invoked in
  rpl_gtid.h, hence the early forward declaration.
*/
static void error(const char *format, ...)
  MY_ATTRIBUTE((format(printf, 1, 2)));
static void warning(const char *format, ...)
  MY_ATTRIBUTE((format(printf, 1, 2)));

#include "rpl_gtid.h"
#include "log_event.h"
#include "log_event_old.h"
#include "rpl_constants.h"
#include "sql_common.h"
#include <mysql/my_dir.h>
#include "welcome_copyright_notice.h" // ORACLE_WELCOME_COPYRIGHT_NOTICE
#include "sql_string.h"
#include "my_decimal.h"

#include <algorithm>
#include <utility>
#include <map>

using std::min;
using std::max;

static FILE *binary_log_index_file;


/**
  The function represents Log_event delete wrapper
  to reset possibly active temp_buf member.
  It's to be invoked in context where the member is
  not bound with dynamically allocated memory and therefore can
  be reset as simple as with plain assignment to NULL.

  @param ev  a pointer to Log_event instance
*/
inline void reset_temp_buf_and_delete(Log_event *ev)
{
  ev->temp_buf= NULL;
  delete ev;
}

/*
  The character set used should be equal to the one used in mysqld.cc for
  server rewrite-db
*/
#define mysqld_charset &my_charset_latin1

#define CLIENT_CAPABILITIES	(CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_LOCAL_FILES)

char server_version[SERVER_VERSION_LENGTH];
ulong filter_server_id = 0;

/*
  This strucure is used to store the event and the log postion of the events 
  which is later used to print the event details from correct log postions.
  The Log_event *event is used to store the pointer to the current event and 
  the event_pos is used to store the current event log postion.
*/
struct buff_event_info
{
  Log_event *event;
  my_off_t event_pos;
};

/* 
  One statement can result in a sequence of several events: Intvar_log_events,
  User_var_log_events, and Rand_log_events, followed by one
  Query_log_event. If statements are filtered out, the filter has to be
  checked for the Query_log_event. So we have to buffer the Intvar,
  User_var, and Rand events and their corresponding log postions until we see 
  the Query_log_event. This dynamic array buff_ev is used to buffer a structure 
  which stores such an event and the corresponding log position.
*/
typedef Prealloced_array<buff_event_info, 16, true> Buff_ev;
Buff_ev *buff_ev(PSI_NOT_INSTRUMENTED);

// needed by net_serv.c
ulong bytes_sent = 0L, bytes_received = 0L;
//ulong mysqld_net_retry_count = 10L;
ulong open_files_limit;
ulong opt_binlog_rows_event_max_size;
uint test_flags = 0; 
static uint opt_protocol= 0;
static FILE *result_file;
int result_file_no;


#ifndef DBUG_OFF
//static const char* default_dbug_option = "d:t:o,/tmp/mysqlbinlog.trace";
#endif
//static const char *load_default_groups[]= { "mysqlbinlog","client",0 };

//static my_bool one_database=0, disable_log_bin= 0;
//static my_bool one_database=0;
//static my_bool opt_hexdump= 0;
const char *base64_output_mode_names[]=
{"NEVER", "AUTO", "UNSPEC", "DECODE-ROWS", NullS};
TYPELIB base64_output_mode_typelib=
  { array_elements(base64_output_mode_names) - 1, "",
    base64_output_mode_names, NULL };
static enum_base64_output_mode opt_base64_output_mode= BASE64_OUTPUT_UNSPEC;
//static char *opt_base64_output_mode_str= 0;
//static my_bool opt_remote_alias= 0;
const char *remote_proto_names[]=
{"BINLOG-DUMP-NON-GTIDS", "BINLOG-DUMP-GTIDS", NullS};
TYPELIB remote_proto_typelib=
  { array_elements(remote_proto_names) - 1, "",
    remote_proto_names, NULL };
static enum enum_remote_proto {
  BINLOG_DUMP_NON_GTID= 0,
  BINLOG_DUMP_GTID= 1,
  BINLOG_LOCAL= 2
} opt_remote_proto= BINLOG_LOCAL;
//static char *opt_remote_proto_str= 0;
static char *database= 0;
static char *output_file= 0;
static char *rewrite= 0;
//static my_bool force_opt= 0, short_form= 0, idempotent_mode= 0;
static my_bool force_opt= 0, short_form= 0;
//static my_bool debug_info_flag, debug_check_flag;
//static my_bool force_if_open_opt= 1, raw_mode= 0;
static my_bool raw_mode= 0;
static my_bool to_last_remote_log= 0, stop_never= 0;
static my_bool opt_verify_binlog_checksum= 1;
//static ulonglong offset = 0;
//static int64 stop_never_slave_server_id= -1;
static int64 connection_server_id= -1;
static char* host = 0;
static int port= 0;
static uint my_end_arg;
static const char* sock= 0;
static char *opt_plugin_dir= 0, *opt_default_auth= 0;
//static my_bool opt_secure_auth= TRUE;

#if defined (_WIN32) && !defined (EMBEDDED_LIBRARY)
static char *shared_memory_base_name= 0;
#endif
static char* user = 0;
static char* pass = 0;
static char *opt_bind_addr = NULL;
//static char *charset= 0;

static uint verbose= 0;

static ulonglong start_position=4, stop_position;
#define start_position_mot ((my_off_t)start_position)
#define stop_position_mot  ((my_off_t)stop_position)
static MYSQL* mysql = NULL;
static char* dirname_for_local_load= 0;
static uint opt_server_id_bits = 0;
static ulong opt_server_id_mask = 0;
Sid_map *global_sid_map= NULL;
Checkable_rwlock *global_sid_lock= NULL;
Gtid_set *gtid_set_included= NULL;
Gtid_set *gtid_set_excluded= NULL;


/**
  Pointer to the Format_description_log_event of the currently active binlog.

  This will be changed each time a new Format_description_log_event is
  found in the binlog. It is finally destroyed at program termination.
*/
static Format_description_log_event* glob_description_event= NULL;

/**
  Exit status for functions in this file.
*/


/*
  Options that will be used to filter out events.
*/
//static char *opt_include_gtids_str= NULL,
//            *opt_exclude_gtids_str= NULL;

static char *opt_exclude_gtids_str= NULL;
static my_bool opt_skip_gtids= 0;

static Exit_status dump_remote_log_entries(PRINT_EVENT_INFO *print_event_info,
                                           const char* logname);
static Exit_status dump_single_log(PRINT_EVENT_INFO *print_event_info,
                                   const char* logname);
static Exit_status dump_multiple_logs(int argc, char **argv);
static Exit_status safe_connect();

struct buff_event_info buff_event;


/**
  Indicates whether the given database should be filtered out,
  according to the --database=X option.

  @param log_dbname Name of database.

  @return nonzero if the database with the given name should be
  filtered out, 0 otherwise.
*/


/**
  Checks whether the given event should be filtered out,
  according to the include-gtids, exclude-gtids and
  skip-gtids options.

  @param ev Pointer to the event to be checked.

  @return true if the event should be filtered out,
          false, otherwise.
*/


/**
  Auxiliary function used by error() and warning().

  Prints the given text (normally "WARNING: " or "ERROR: "), followed
  by the given vprintf-style string, followed by a newline.

  @param format Printf-style format string.
  @param args List of arguments for the format string.
  @param msg Text to print before the string.
*/
static void error_or_warning(const char *format, va_list args, const char *msg)
{
  fprintf(stderr, "%s: ", msg);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
}

/**
  Prints a message to stderr, prefixed with the text "ERROR: " and
  suffixed with a newline.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void error(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "ERROR");
  va_end(args);
}


/**
  Prints a message to stderr, prefixed with the text "WARNING: " and
  suffixed with a newline.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void warning(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "WARNING");
  va_end(args);
}

/**
  Frees memory for global variables in this file.
*/
static void cleanup()
{
//  my_free(pass);
//  my_free(database);
  my_free(rewrite);
//  my_free(host);
//  my_free(user);
  my_free(dirname_for_local_load);
  if(pass)
  {
    delete pass;
  }
  if(database)
  {
    delete database;
  }

  if(host)
  {
    delete host;
  }

  if(user)
  {
    delete user;
  }

  if(opt_exclude_gtids_str)
  {
    free(opt_exclude_gtids_str);
  }


  for (size_t i= 0; i < buff_ev->size(); i++)
  {
    buff_event_info pop_event_array= buff_ev->at(i);
    delete (pop_event_array.event);
  }
  delete buff_ev;

  delete glob_description_event;
  if (mysql)
    mysql_close(mysql);
}


/**
  Create and initialize the global mysql object, and connect to the
  server.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status safe_connect()
{
  /*
    A possible old connection's resources are reclaimed now
    at new connect attempt. The final safe_connect resources
    are mysql_closed at the end of program, explicitly.
  */
  if(mysql)
  {
    if(mysql_ping(mysql) ==0 )
    {
      mysql_close(mysql);
    }
  }

  mysql= mysql_init(NULL);

  if (!mysql)
  {
    sql_print_warning("Failed on mysql_init.");
    return ERROR_STOP;
  }

  if (opt_plugin_dir && *opt_plugin_dir)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opt_plugin_dir);

  if (opt_default_auth && *opt_default_auth)
    mysql_options(mysql, MYSQL_DEFAULT_AUTH, opt_default_auth);

  if (opt_protocol)
    mysql_options(mysql, MYSQL_OPT_PROTOCOL, (char*) &opt_protocol);
  if (opt_bind_addr)
    mysql_options(mysql, MYSQL_OPT_BIND, opt_bind_addr);
  if(net_read_time_out)
  {
    mysql_options(mysql,MYSQL_OPT_READ_TIMEOUT,&net_read_time_out);
  }
  int opt_connect_timeout=2;
  mysql_options(mysql,MYSQL_OPT_CONNECT_TIMEOUT,&opt_connect_timeout);
#if defined (_WIN32) && !defined (EMBEDDED_LIBRARY)
  if (shared_memory_base_name)
    mysql_options(mysql, MYSQL_SHARED_MEMORY_BASE_NAME,
                  shared_memory_base_name);
#endif
  mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_RESET, 0);
  mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                 "program_name", "mysqlbinlog");
  mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                "_client_role", "binary_log_listener");

  if (!mysql_real_connect(mysql, host, user, pass, 0, port, sock, 0))
  {
    sql_print_error("Failed on connect: %s", mysql_error(mysql));
    return ERROR_STOP;
  }
  mysql->reconnect= 1;
  return OK_CONTINUE;
}


/**
  High-level function for dumping a named binlog.

  This function calls dump_remote_log_entries() or
  dump_local_log_entries() to do the job.

  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status dump_single_log(PRINT_EVENT_INFO *print_event_info,
                                   const char* logname)
{
  DBUG_ENTER("dump_single_log");

  Exit_status rc= OK_CONTINUE;

  switch (opt_remote_proto)
  {
    case BINLOG_DUMP_NON_GTID:
    case BINLOG_DUMP_GTID:
      rc= dump_remote_log_entries(print_event_info, logname);
    break;
    default:
      DBUG_ASSERT(0);
    break;
  }
  return rc;
}


static Exit_status dump_multiple_logs(int argc, char **argv)
{
  DBUG_ENTER("dump_multiple_logs");
  Exit_status rc= OK_CONTINUE;

  PRINT_EVENT_INFO print_event_info;
  if (!print_event_info.init_ok())
    DBUG_RETURN(ERROR_STOP);
  /*
     Set safe delimiter, to dump things
     like CREATE PROCEDURE safely
  */
  my_stpcpy(print_event_info.delimiter, "/*!*/;");
  
  print_event_info.verbose= short_form ? 0 : verbose;
  print_event_info.short_form= short_form;
  print_event_info.base64_output_mode= opt_base64_output_mode;
  print_event_info.skip_gtids= opt_skip_gtids;

  // Dump all logs.
  my_off_t save_stop_position= stop_position;
  stop_position= ~(my_off_t)0;
  stop_position = save_stop_position;
  const char* start_binlog_file="mysql-bin-000001";
  start_position= BIN_LOG_HEADER_SIZE;
  if((rc = dump_single_log(&print_event_info,start_binlog_file)) != OK_CONTINUE)
  {
    sql_print_error("Dump remote log error");
  }

  if (!buff_ev->empty())
    sql_print_warning("The range of printed events ends with an Intvar_event, "
            "Rand_event or User_var_event with no matching Query_log_event. "
            "This might be because the last statement was not fully written "
            "to the log, or because you are using a --stop-position or "
            "--stop-datetime that refers to an event in the middle of a "
            "statement. The event(s) from the partial statement have not been "
            "written to output. ");

  else if (print_event_info.have_unflushed_events)
    sql_print_warning("The range of printed events ends with a row event or "
            "a table map event that does not have the STMT_END_F "
            "flag set. This might be because the last statement "
            "was not fully written to the log, or because you are "
            "using a --stop-position or --stop-datetime that refers "
            "to an event in the middle of a statement. The event(s) "
            "from the partial statement have not been written to output.");

  /* Set delimiter back to semicolon */
  return(rc);
}


/**
  When reading a remote binlog, this function is used to grab the
  Format_description_log_event in the beginning of the stream.
  
  This is not as smart as check_header() (used for local log); it will
  not work for a binlog which mixes format. TODO: fix this.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status check_master_version()
{
  DBUG_ENTER("check_master_version");
  MYSQL_RES* res = 0;
  MYSQL_ROW row;
  const char* version;

  if (mysql_query(mysql, "SELECT VERSION()") ||
      !(res = mysql_store_result(mysql)))
  {
    sql_print_error("Could not find server version: "
          "Query failed when checking master version: %s", mysql_error(mysql));
    DBUG_RETURN(ERROR_STOP);
  }
  if (!(row = mysql_fetch_row(res)))
  {
    sql_print_error("Could not find server version: "
          "Master returned no rows for SELECT VERSION().");
    goto err;
  }

  if (!(version = row[0]))
  {
    sql_print_error("Could not find server version: "
          "Master reported NULL for the version.");
    goto err;
  }
  /* 
     Make a notice to the server that this client
     is checksum-aware. It does not need the first fake Rotate
     necessary checksummed. 
     That preference is specified below.
  */
  if (mysql_query(mysql, "SET @master_binlog_checksum='NONE'"))
  {
    sql_print_error("Could not notify master about checksum awareness."
          "Master returned '%s'", mysql_error(mysql));
    goto err;
  }
  delete glob_description_event;
  switch (*version) {
  case '3':
    glob_description_event= new Format_description_log_event(1);
    break;
  case '4':
    glob_description_event= new Format_description_log_event(3);
    break;
  case '5':
    /*
      The server is soon going to send us its Format_description log
      event, unless it is a 5.0 server with 3.23 or 4.0 binlogs.
      So we first assume that this is 4.0 (which is enough to read the
      Format_desc event if one comes).
    */
    glob_description_event= new Format_description_log_event(3);
    break;
  default:
    glob_description_event= NULL;
    sql_print_error("Could not find server version: "
          "Master reported unrecognized MySQL version '%s'.", version);
    goto err;
  }
  if (!glob_description_event || !glob_description_event->is_valid())
  {
    sql_print_error("Failed creating Format_description_log_event; out of memory?");
    goto err;
  }

  mysql_free_result(res);
  DBUG_RETURN(OK_CONTINUE);

err:
  mysql_free_result(res);
  DBUG_RETURN(ERROR_STOP);
}


static int get_dump_flags()
{
  return stop_never ? 0 : BINLOG_DUMP_NON_BLOCK;
}

typedef struct Binlog_relay_IO_param {
    uint32 server_id;
    my_thread_id thread_id;

    /* Channel name */
    char* channel_name;

    /* Master host, user and port */
    char *host;
    char *user;
    unsigned int port;

    char *master_log_name;
    my_off_t master_log_pos;

    MYSQL *mysql;                        /* the connection to master */
} Binlog_relay_IO_param;

Binlog_relay_IO_param* binlogRelayIoParam;

/**
  Requests binlog dump from a remote server and prints the events it
  receives.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.
  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status dump_remote_log_entries(PRINT_EVENT_INFO *print_event_info,
                                           const char* logname)
{
  const char *error_msg= NULL;
  Log_event *ev= NULL;
  Log_event_type type= binary_log::UNKNOWN_EVENT;
  uchar *command_buffer= NULL;
  size_t command_size= 0;
  ulong len= 0;
  unsigned long int total_bytes=0;
  size_t tlen = strlen(logname);
  size_t BINLOG_NAME_INFO_SIZE = tlen;
 // size_t logname_len= 0;
  uint server_id= 0;
  NET* net= NULL;
  my_off_t old_off= start_position_mot;
//  char fname[FN_REFLEN + 1];
  char log_file_name[FN_REFLEN + 1];
  Exit_status retval= OK_CONTINUE;
  enum enum_server_command command= COM_END;
//  fname[0]= log_file_name[0]= 0;
  log_file_name[0]= 0;


  if (tlen > UINT_MAX)
  {
    sql_print_error("Log name too long.");
    return ERROR_STOP;
  }



  /*
    Even if we already read one binlog (case of >=2 binlogs on command line),
    we cannot re-use the same connection as before, because it is now dead
    (COM_BINLOG_DUMP kills the thread when it finishes).
  */

  vs_reconnect:
  if ((retval= safe_connect()) != OK_CONTINUE)
  {
    return retval;
  }
  net= &mysql->net;

  if ((retval= check_master_version()) != OK_CONTINUE)
  {
    return retval;
  }

  if((retval=get_master_uuid()) != OK_CONTINUE)
  {
    return retval;
  }

  if(switched && recovery_mode) //master changed;
  {
    //todo switch hook;
    if((retval = get_executed_gtid()) != OK_CONTINUE)
    {
      return retval;
    }
    if((retval = set_gtid_executed()) != OK_CONTINUE)
    {
      return retval;
    }
    opt_remote_proto = BINLOG_DUMP_GTID;
    binlog_file_open_mode = O_WRONLY|O_BINARY ; //overwrite
  }
  else if(recovery_mode && !switched)
  {
    opt_remote_proto = BINLOG_DUMP_NON_GTID;
    BINLOG_NAME_INFO_SIZE = strlen(new_binlog_file_name); //新的文件
    if(!re_connect_start_position)
    {
      re_connect_start_position = (start_position > respond_pos ? start_position:respond_pos);
    }
    //re_connect_start_position = (start_position > respond_pos ? start_position:respond_pos);
    binlog_file_open_mode = O_WRONLY | FAPPEND |O_BINARY ;

  }

  if (connection_server_id != -1)
  {
    server_id= static_cast<uint>(connection_server_id);
  }

  binlogRelayIoParam = new Binlog_relay_IO_param;
  binlogRelayIoParam->channel_name=strdup("test");
  binlogRelayIoParam->host=host;
  binlogRelayIoParam->user=user;
  binlogRelayIoParam->server_id=server_id;
  binlogRelayIoParam->thread_id = 1;
  binlogRelayIoParam->master_log_name =NULL;
  binlogRelayIoParam->master_log_pos=0;
  binlogRelayIoParam->mysql = mysql;

  if(handle_repl_semi_slave_request_dump((void*)binlogRelayIoParam,0))
  {
    sql_print_error("call repl_semi_slave_request_dump error");
  }

  if (opt_remote_proto == BINLOG_DUMP_NON_GTID)
  {
    bool suppress_warnings;
    register_slave_on_master(mysql,&suppress_warnings);
    command= COM_BINLOG_DUMP;
    size_t allocation_size= ::BINLOG_POS_OLD_INFO_SIZE +
      BINLOG_NAME_INFO_SIZE + ::BINLOG_FLAGS_INFO_SIZE +
      ::BINLOG_SERVER_ID_INFO_SIZE + 1;
    if (!(command_buffer= (uchar *) my_malloc(PSI_NOT_INSTRUMENTED,
                                              allocation_size, MYF(MY_WME))))
    {
      sql_print_error("Got fatal error allocating memory.");
      return ERROR_STOP;
    }
    uchar* ptr_buffer= command_buffer;

    /*
      COM_BINLOG_DUMP accepts only 4 bytes for the position, so
      we are forced to cast to uint32.
    */
    int4store(ptr_buffer, (uint32) re_connect_start_position);
    ptr_buffer+= ::BINLOG_POS_OLD_INFO_SIZE;
    int2store(ptr_buffer, get_dump_flags());
    ptr_buffer+= ::BINLOG_FLAGS_INFO_SIZE;
    int4store(ptr_buffer, server_id);
    ptr_buffer+= ::BINLOG_SERVER_ID_INFO_SIZE;
    memcpy(ptr_buffer, new_binlog_file_name, BINLOG_NAME_INFO_SIZE);
    ptr_buffer+= BINLOG_NAME_INFO_SIZE;

    command_size= ptr_buffer - command_buffer;
    DBUG_ASSERT(command_size == (allocation_size - 1));
  }
  else
  {

    bool suppress_warnings;
    register_slave_on_master(mysql,&suppress_warnings);
    command= COM_BINLOG_DUMP_GTID;
    char real_log_name[]="";
    BINLOG_NAME_INFO_SIZE= strlen(real_log_name);
    global_sid_lock->rdlock();

    // allocate buffer
    size_t encoded_data_size= gtid_set_excluded->get_encoded_length();
    size_t allocation_size=
            ::BINLOG_FLAGS_INFO_SIZE + ::BINLOG_SERVER_ID_INFO_SIZE +
            ::BINLOG_NAME_SIZE_INFO_SIZE + BINLOG_NAME_INFO_SIZE +
            ::BINLOG_POS_INFO_SIZE + ::BINLOG_DATA_SIZE_INFO_SIZE +
            encoded_data_size + 1;

    if (!(command_buffer= (uchar *) my_malloc(PSI_NOT_INSTRUMENTED,
                                              allocation_size, MYF(MY_WME))))
    {
      sql_print_error("Got fatal error allocating memory.");
      global_sid_lock->unlock();
      return ERROR_STOP;
    }
    uchar* ptr_buffer= command_buffer;
    int2store(ptr_buffer, get_dump_flags());
    ptr_buffer+= ::BINLOG_FLAGS_INFO_SIZE;
    int4store(ptr_buffer, server_id);
    ptr_buffer+= ::BINLOG_SERVER_ID_INFO_SIZE;
    int4store(ptr_buffer, static_cast<uint32>(BINLOG_NAME_INFO_SIZE));
    ptr_buffer+= ::BINLOG_NAME_SIZE_INFO_SIZE;
    memcpy(ptr_buffer, logname, BINLOG_NAME_INFO_SIZE);
    ptr_buffer+= BINLOG_NAME_INFO_SIZE;
    int8store(ptr_buffer, start_position);
    ptr_buffer+= ::BINLOG_POS_INFO_SIZE;
    int4store(ptr_buffer, static_cast<uint32>(encoded_data_size));
    ptr_buffer+= ::BINLOG_DATA_SIZE_INFO_SIZE;
    gtid_set_excluded->encode(ptr_buffer);
    ptr_buffer+= encoded_data_size;

    global_sid_lock->unlock();

    command_size= ptr_buffer - command_buffer;
    DBUG_ASSERT(command_size == (allocation_size - 1));
  }

  if (simple_command(mysql, command, command_buffer, command_size, 1))
  {
    sql_print_information("Got fatal error sending the log dump command.");
    my_free(command_buffer);
    return ERROR_STOP;
  }
  re_connect_start_position = 0;
  my_free(command_buffer);

  const char* event_buf;
  for(;;)
  {
    //recovery mode read.
    if(recovery_mode)
    {
      len = cli_safe_read(mysql, NULL);
      if (len == packet_error)
      {
        sql_print_error("Got error reading packet from server: %s,%i", mysql_error(mysql),mysql_errno(mysql));
        //maybe that master execute reset master. reset respond_pos and new_binlog_file_name.
        if(mysql_errno(mysql) == ER_MASTER_FATAL_ERROR_READING_BINLOG)
        {
          respond_pos = 0;
          memset(new_binlog_file_name,0,sizeof(new_binlog_file_name));
        }
        recovery_mode=true;
        like_reset_slave();

        goto vs_reconnect;
      }
      len--;

      event_buf= (const char *) net->read_pos + 1;
      if(handle_repl_semi_slave_read_event((void*)binlogRelayIoParam,(char*)net->read_pos+1,len,&event_buf,&len))
      {
        sql_print_error("call handle_repl_semi_slave_read_event error");
      }
      type=(Log_event_type)event_buf[EVENT_TYPE_OFFSET];
      if(type == binary_log::HEARTBEAT_LOG_EVENT)
      {
        sql_print_information("recovery mode,received HEARTBEAT log event");
        continue;
      }

      if (!(ev= Log_event::read_log_event(event_buf,
                                          len, &error_msg,
                                          glob_description_event,
                                          opt_verify_binlog_checksum)))
      {
        sql_print_error("Could not construct log event object in reconnect mode: %s,event len: %lu", error_msg,len);
        return ERROR_STOP;
      }
      /*
        If reading from a remote host, ensure the temp_buf for the
        Log_event class is pointing to the incoming stream.
      */
      ev->register_temp_buf((char*)event_buf);

      if (type == binary_log::ROTATE_EVENT)
      {
        Rotate_log_event *rev= (Rotate_log_event *)ev;

        if(strcmp(new_binlog_file_name,rev->new_log_ident) ==0 )
        {
          //无用的ROTATE_EVENT
          reset_temp_buf_and_delete(rev);
          continue;
        }
        else
        {
          //可能恢复模式正好在日志轮换阶段,切换到正常读取模式
          recovery_mode = false;
          goto normal_event;
        }
      }
      else if(type == binary_log::FORMAT_DESCRIPTION_EVENT || type == binary_log:: PREVIOUS_GTIDS_LOG_EVENT)
      {
        delete glob_description_event;
        glob_description_event= (Format_description_log_event*) ev;
        print_event_info->common_header_len= glob_description_event->common_header_len;
        ev->temp_buf= 0;
        ev= 0;
      }
      else
      {
        if (!(result_file = my_fopen(new_binlog_file_name, binlog_file_open_mode,
                                     MYF(MY_WME))))
        {
          sql_print_error("Could not create log file '%s'", new_binlog_file_name);
          return ERROR_STOP;
        }
        recovery_mode=false;
        goto normal_event;
      }
    }
    else
    {
      break;
    }
  }

  for (;;)
  {
    //normal read.
    len = cli_safe_read(mysql, NULL);
    if (len == packet_error)
    {
      sql_print_error("Got error reading packet from server: %i,%s", mysql_errno(mysql),mysql_error(mysql));
      recovery_mode=true;
      goto vs_reconnect;
    }
    len--;
    if (len < 8 && net->read_pos[0] == 254)
    {
      sql_print_error("Got error reading packet from server: %i,%s",mysql_errno(mysql),mysql_error(mysql));
      recovery_mode=true;
      goto vs_reconnect;
    }
    //  break; // end of data
//      DBUG_PRINT("info",( "len: %lu  net->read_pos[5]: %d\n",
//			len, net->read_pos[5]));
    /*
      In raw mode We only need the full event details if it is a 
      ROTATE_EVENT or FORMAT_DESCRIPTION_EVENT
    */

    event_buf= (const char *) net->read_pos + 1;
    if(handle_repl_semi_slave_read_event((void*)binlogRelayIoParam,(char*)net->read_pos+1,len,&event_buf,&len))
    {
      sql_print_error("call handle_repl_semi_slave_read_event error");
    }
    type=(Log_event_type)event_buf[EVENT_TYPE_OFFSET];

    if (type == binary_log::HEARTBEAT_LOG_EVENT)
    {
      sql_print_information("received HEARTBEAT log event");
      continue;
    }


    if (!(ev= Log_event::read_log_event(event_buf,
                                        len, &error_msg,
                                        glob_description_event,
                                        opt_verify_binlog_checksum)))
    {
      sql_print_error("Could not construct log event object: %s", error_msg);
      return ERROR_STOP;
    }
    /*
      If reading from a remote host, ensure the temp_buf for the
      Log_event class is pointing to the incoming stream.
    */
    ev->register_temp_buf((char*)event_buf);
    respond_pos = ev->common_header->log_pos;

    normal_event:
    /*
      If this is a Rotate event, maybe it's the end of the requested binlog;
      in this case we are done (stop transfer).
      This is suitable for binlogs, not relay logs (but for now we don't read
      relay logs remotely because the server is not able to do that). If one
      day we read relay logs remotely, then we will have a problem with the
      detection below: relay logs contain Rotate events which are about the
      binlogs, so which would trigger the end-detection below.
    */
    if (type == binary_log::ROTATE_EVENT)
    {
      // error("last total bytes %lu",total_bytes);
      total_bytes =0;
      Rotate_log_event *rev= (Rotate_log_event *)ev;
      /*
        If this is a fake Rotate event, and not about our log, we can stop
        transfer. If this a real Rotate event (so it's not about our log,
        it's in our log describing the next log), we print it (because it's
        part of our log) and then we will stop when we receive the fake one
        soon.
      */

      if (output_file != 0)
      {
        my_snprintf(log_file_name, sizeof(log_file_name), "%s",
                    rev->new_log_ident);
        memset(new_binlog_file_name,0,(FN_REFLEN + 1));
        my_stpcpy(new_binlog_file_name, rev->new_log_ident);
      }
      else
      {
        my_stpcpy(log_file_name, rev->new_log_ident);
        memset(new_binlog_file_name,0,(FN_REFLEN + 1));
        my_stpcpy(new_binlog_file_name, rev->new_log_ident);
      }

      if (rev->common_header->when.tv_sec == 0)
      {
        if (!to_last_remote_log)
        {
//            if ((rev->ident_len != logname_len) ||
//                memcmp(rev->new_log_ident, logname, logname_len))
//            {
//              reset_temp_buf_and_delete(rev);
//              DBUG_RETURN(OK_CONTINUE);
//            }
          /*
            Otherwise, this is a fake Rotate for our log, at the very
            beginning for sure. Skip it, because it was not in the original
            log. If we are running with to_last_remote_log, we print it,
            because it serves as a useful marker between binlogs then.
          */
          reset_temp_buf_and_delete(rev);
          continue;
        }
        /*
           Reset the value of '# at pos' field shown against first event of
           next binlog file (fake rotate) picked by mysqlbinlog --to-last-log
       */
        old_off= start_position_mot;
        len= 0; // fake Rotate, so don't increment old_off /*ashe note: this len is real buf len to write,so 0*/
      }
    }
    else if (type == binary_log::FORMAT_DESCRIPTION_EVENT)
    {
      /*
        This could be an fake Format_description_log_event that server
        (5.0+) automatically sends to a slave on connect, before sending
        a first event at the requested position.  If this is the case,
        don't increment old_off. Real Format_description_log_event always
        starts from BIN_LOG_HEADER_SIZE position.
      */
      // fake event when not in raw mode, don't increment old_off
      if ((old_off != BIN_LOG_HEADER_SIZE) && (!raw_mode))
        len= 1;

      if (result_file && (result_file != stdout))
        my_fclose(result_file, MYF(0));
      if (!(result_file = my_fopen(log_file_name, binlog_file_open_mode,
                                   MYF(MY_WME))))
      {
        sql_print_error("Could not create log file '%s'", log_file_name);
        return ERROR_STOP;
      }
      result_file_no = fileno(result_file);

      DBUG_EXECUTE_IF("simulate_result_file_write_error_for_FD_event",
                      DBUG_SET("+d,simulate_fwrite_error"););
      if (my_fwrite(result_file, (const uchar*) BINLOG_MAGIC,
                    BIN_LOG_HEADER_SIZE, MYF(MY_NABP)))
      {
        sql_print_error("Could not write into log file '%s'", log_file_name);
        return ERROR_STOP;
      }
      //setbuf(result_file,NULL);
      //write index file
      if(my_fwrite(binary_log_index_file,(const uchar*)new_binlog_file_name,
              strlen(log_file_name),MYF(MY_NABP)))
      {
        sql_print_error("Could not write into log index file '%s'", index_file_name);
        return ERROR_STOP;
      }


      if(my_fwrite(binary_log_index_file,(const uchar*)line_b,
                   strlen(line_b),MYF(MY_NABP)))
      {
        sql_print_error("Could not write into log index file '%s'", index_file_name);
        return ERROR_STOP;
      }

      if(fflush(binary_log_index_file))
      {
        //todo log here
        sql_print_error("fflush binary log index file %s failed",index_file_name);
        return ERROR_STOP;
      }

      total_bytes+=4; //BINLOG_MAGIC is 4 bytes.

      /*
        Need to handle these events correctly in raw mode too
        or this could get messy
      */
      delete glob_description_event;
      glob_description_event= (Format_description_log_event*) ev;
      print_event_info->common_header_len= glob_description_event->common_header_len;
      ev->temp_buf= 0;
      ev= 0;
    }

    if (type == binary_log::LOAD_EVENT)
    {
      DBUG_ASSERT(raw_mode);
      sql_print_warning("Attempting to load a remote pre-4.0 binary log that contains "
              "LOAD DATA INFILE statements. The file will not be copied from "
              "the remote server. ");
    }

    if (my_fwrite(result_file, (const uchar*)event_buf, len, MYF(MY_NABP)))
    {
      sql_print_error("Could not write into log file '%s'", log_file_name);
      retval= ERROR_STOP;
    }
    total_bytes += len;
    if (ev)
      reset_temp_buf_and_delete(ev);
    if (retval != OK_CONTINUE)
    {
      return retval;
    }

    /*
      Let's adjust offset for remote log as for local log to produce
      similar text and to have --stop-position to work identically.
    */
    old_off+= len-1;
    binlogRelayIoParam->master_log_name = new_binlog_file_name;
    binlogRelayIoParam->master_log_pos = respond_pos;

    if(semi_sync_need_reply)
    {
      if(fflush(result_file))
      {
        sql_print_error("fflush file %s failed",log_file_name);
        return ERROR_STOP;
      }
      if(fsync_mode)
      {
        if(fsync(result_file_no))
        {
          sql_print_error("Sync file %s failed",log_file_name);
          return ERROR_STOP;
        }
      }

    }

    //ack
    handle_repl_semi_slave_queue_event((void*)binlogRelayIoParam,event_buf,0,0);

  }

  return OK_CONTINUE;
}


/**
   GTID cleanup destroys objects and reset their pointer.
   Function is reentrant.
*/
inline void gtid_client_cleanup()
{
  delete global_sid_lock;
  delete global_sid_map;
  delete gtid_set_excluded;
  delete gtid_set_included;
  global_sid_lock= NULL;
  global_sid_map= NULL;
  gtid_set_excluded= NULL;
  gtid_set_included= NULL;
}

/**
   GTID initialization.

   @return true if allocation does not succeed
           false if OK
*/
inline bool gtid_client_init()
{
  bool res=
    (!(global_sid_lock= new Checkable_rwlock) ||
     !(global_sid_map= new Sid_map(global_sid_lock)) ||
     !(gtid_set_excluded= new Gtid_set(global_sid_map)) ||
     !(gtid_set_included= new Gtid_set(global_sid_map)));
  if (res)
  {
    gtid_client_cleanup();
  }
  return res;
}


int main(int argc, char** argv)
{
  //char **defaults_argv;
  Exit_status retval= OK_CONTINUE;
  MY_INIT(argv[0]);
  DBUG_ENTER("main");
  DBUG_PROCESS(argv[0]);

  my_init_time(); // for time functions
  tzset(); // set tzname
  /*
    A pointer of type Log_event can point to
     INTVAR
     USER_VAR
     RANDOM
    events.
  */
  buff_ev= new Buff_ev(PSI_NOT_INSTRUMENTED);

  if(argc != 2)
  {
    std::cout << ("Wrong usage.\nusage: virtual_slave virtual_slave.cnf") << std::endl;
    exit(1);
  }

  //read config file.
  Config virtual_slave_config(argv[1]);

  string _s_user = virtual_slave_config.Read("master_user",_s_user);
  user = string_to_char(_s_user);

  string _s_host = virtual_slave_config.Read("master_host",_s_host);
  host = string_to_char(_s_host);

  port = virtual_slave_config.Read("master_port",0);

  string _s_pass = virtual_slave_config.Read("master_password",_s_pass);
  pass = string_to_char(_s_pass);

  heartbeat_period = virtual_slave_config.Read("heartbeat_period",0);
  net_read_time_out = virtual_slave_config.Read("net_read_time_out",0);

  get_start_gtid_mode = virtual_slave_config.Read("get_start_gtid_mode",0);
  connection_server_id = virtual_slave_config.Read("virtual_slave_server_id",0);
  raw_mode = 1;
  stop_never = 1;

  string _s_output_file = virtual_slave_config.Read("binlog_dir",_s_output_file);
  output_file = string_to_char(_s_output_file);
  string _s_opt_exclude_gtids_str = virtual_slave_config.Read("exclude_gtids",_s_opt_exclude_gtids_str);
  opt_exclude_gtids_str = strdup(_s_opt_exclude_gtids_str.data());
  virtual_slave_log_file = strdup("virtual_slave.log");
  log_level = virtual_slave_config.Read("log_level",0);
  fsync_mode = virtual_slave_config.Read("fsync_mode",0);

  binlog_file_open_mode = O_WRONLY | O_BINARY;
  respond_pos = 0;
  switched = false;
  recovery_mode =false;
  re_connect_start_position=0;

  if(prepare_log_file(virtual_slave_log_file) != OK_CONTINUE)
  {
    return 1;
  }

  if(symisync_slave_init())
  {
    sql_print_error("init semisync_slave plugin error");
    return 1;
  }

  if (gtid_client_init())
  {
    sql_print_error("Could not initialize GTID structuress.");
    exit(1);
  }

  umask(((~my_umask) & 0666));
  /* Check for argument conflicts and do any post-processing */
//  if (determine_dump_mode() == ERROR_STOP)
//    exit(1);

  opt_server_id_mask = (opt_server_id_bits == 32)?
    ~ ulong(0) : (1 << opt_server_id_bits) -1;

  my_set_max_open_files(open_files_limit);

  MY_TMPDIR tmpdir;
  tmpdir.list= 0;
  if (!dirname_for_local_load)
  {
    if (init_tmpdir(&tmpdir, 0))
      exit(1);
    dirname_for_local_load= my_strdup(PSI_NOT_INSTRUMENTED,
                                      my_tmpdir(&tmpdir), MY_WME);
  }

  if(open_index_file() == ERROR_STOP)
  {
    return 1;
  }

  if (determine_dump_mode() == ERROR_STOP)
  {
    return 1;
  }
  retval= dump_multiple_logs(argc, argv);
  if (tmpdir.list)
  {
    free_tmpdir(&tmpdir);
  }

  if (result_file && (result_file != stdout))
  {
    my_fclose(result_file, MYF(0));
  }
  cleanup();

  my_free_open_file_info();
  /* We cannot free DBUG, it is used in global destructors after exit(). */
  my_end(my_end_arg | MY_DONT_FREE_DBUG);
  gtid_client_cleanup();

  //exit(retval == ERROR_STOP ? 1 : 0);
  /* Keep compilers happy. */
  DBUG_RETURN(retval == ERROR_STOP ? 1 : 0);
}

/*
  We must include this here as it's compiled with different options for
  the server
*/

//#include "decimal.c"
#include "my_decimal.cc"
#include "log_event.cc"
#include "log_event_old.cc"
#include "rpl_utility.cc"
#include "rpl_gtid_sid_map.cc"
#include "rpl_gtid_misc.cc"
#include "rpl_gtid_set.cc"
#include "rpl_gtid_specification.cc"
#include "rpl_tblmap.cc"


/**
  Faster net_store_length when we know that length is less than 65536.
  We keep a separate version for that range because it's widely used in
  libmysql.

  uint is used as agrument type because of MySQL type conventions:
    - uint for 0..65536
    - ulong for 0..4294967296
    - ulonglong for bigger numbers.
*/

static uchar *net_store_length_fast(uchar *packet, size_t length)
{
  if (length < 251)
  {
    *packet=(uchar) length;
    return packet+1;
  }
  *packet++=252;
  int2store(packet,(uint) length);
  return packet+2;
}

/****************************************************************************
  Functions used by the protocol functions (like net_send_ok) to store
  strings and numbers in the header result packet.
****************************************************************************/

/* The following will only be used for short strings < 65K */

uchar *net_store_data(uchar *to, const uchar *from, size_t length)
{
  to=net_store_length_fast(to,length);
  memcpy(to,from,length);
  return to+length;
}

int register_slave_on_master(MYSQL* mysql,/* Master_info *mi,*/
                             bool *suppress_warnings)
{
  uchar buf[1024], *pos= buf;
  size_t report_host_len=0, report_user_len=0, report_password_len=0;
  DBUG_ENTER("register_slave_on_master");

  *suppress_warnings= FALSE;
  if (report_host)
    report_host_len= strlen(report_host);
  if (report_host_len > HOSTNAME_LENGTH)
  {
    // todo log here
    sql_print_warning("The length of report_host is %zu. "
                              "It is larger than the max length(%d), so this "
                              "slave cannot be registered to the master",
                      report_host_len, HOSTNAME_LENGTH);
    DBUG_RETURN(0);
  }

  if (report_user)
    report_user_len= strlen(report_user);
  if (report_user_len > USERNAME_LENGTH)
  {
    //todo log here
    sql_print_warning("The length of report_user is %zu. "
                              "It is larger than the max length(%d), so this "
                              "slave cannot be registered to the master.",
                      report_user_len, USERNAME_LENGTH);
    DBUG_RETURN(0);
  }

  if (report_password)
    report_password_len= strlen(report_password);
  if (report_password_len > MAX_PASSWORD_LENGTH)
  {
    //todo log here
//    sql_print_warning("The length of report_password is %zu. "
//                              "It is larger than the max length(%d), so this "
//                              "slave cannot be registered to the master%s.",
//                      report_password_len, MAX_PASSWORD_LENGTH,
//                      mi->get_for_channel_str());
    DBUG_RETURN(0);
  }

  int4store(pos,connection_server_id); pos+= 4;
  pos= net_store_data(pos, (uchar*) report_host, report_host_len);
  pos= net_store_data(pos, (uchar*) report_user, report_user_len);
  pos= net_store_data(pos, (uchar*) report_password, report_password_len);
  int2store(pos, (uint16) report_port); pos+= 2;
  /*
    Fake rpl_recovery_rank, which was removed in BUG#13963,
    so that this server can register itself on old servers,
    see BUG#49259.
   */
  int4store(pos, /* rpl_recovery_rank */ 0);    pos+= 4;
  /* The master will fill in master_id */
  int4store(pos, 0);                    pos+= 4;

  if (simple_command(mysql, COM_REGISTER_SLAVE, buf, (size_t) (pos- buf), 0))
  {
    if (mysql_errno(mysql) == ER_NET_READ_INTERRUPTED)
    {
      *suppress_warnings= TRUE;                 // Suppress reconnect warning
    }
//    else if (!check_io_slave_killed(mi->info_thd, mi, NULL))
//    {
//      char buf[256];
//      my_snprintf(buf, sizeof(buf), "%s (Errno: %d)", mysql_error(mysql),
//                  mysql_errno(mysql));
//      mi->report(ERROR_LEVEL, ER_SLAVE_MASTER_COM_FAILURE,
//                 ER(ER_SLAVE_MASTER_COM_FAILURE), "COM_REGISTER_SLAVE", buf);
//    }
    DBUG_RETURN(1);
  }

  if(set_heartbeat_period(mysql) !=0)
  {
    //todo log here
    return -1;
  }

  if(set_slave_uuid(mysql) !=0)
  {
    //todo log here
    return -1;
  }
  DBUG_RETURN(0);
}


/**
 * set replication heartbeat period.
 * @param mysql
 * @return -1 failed; 0 successfully.
 */
int set_heartbeat_period(MYSQL* mysql)
{
  char llbuf[22];
  const char query_format[]= "SET @master_heartbeat_period= %s";
  char query[sizeof(query_format) - 2 + sizeof(llbuf)];
  /*
     the period is an ulonglong of nano-secs.
  */
  llstr((ulonglong) (heartbeat_period*1000000000UL), llbuf);
  sprintf(query, query_format, llbuf);
  if(mysql_real_query(mysql,query,static_cast<ulong>(strlen(query))))
  {
    sql_print_error("%s error %s,%i",query,mysql_error(mysql),mysql_errno(mysql));
    return -1;
  }
  return 0;
}

int set_slave_uuid(MYSQL* mysql)
{
  char* query = new char[100];
  sprintf(query,"SET @slave_uuid= '63cf7450-9829-11e7-8a58-000c2985ca33'");
  if(mysql_real_query(mysql,query,strlen(query)))
  {
    sql_print_error("%s error %s,%i",query,mysql_error(mysql),mysql_errno(mysql));
    delete[] query;
    return -1;
  }
  return 0;
}

char* string_to_char(string str)
{
  char* p  = new char[str.length()+1];
  for(unsigned int i=0;i<str.length();i++)
  {
    p[i]=str[i];
  }
  p[str.length()] = '\0';
  return p;
}


Exit_status determine_dump_mode()
{
  DBUG_ENTER("determine_dump_mode");

  switch(get_start_gtid_mode)
  {
    case 0:
    {
      //0:decide by exclude_gtids config(clean binlog_dir);
      opt_remote_proto = BINLOG_DUMP_GTID;
      int access_res = access(output_file,R_OK|W_OK);
      if(access_res ==0 )
      {
        binlog_file_open_mode = O_WRONLY | O_BINARY;
      }
      else
      {
        //todo log here
        perror("access");
        int mk_res =  mkdir(output_file,S_IRWXU | S_IRGRP | S_IXGRP);
        if(mk_res != 0)
        {
          sql_print_error("create binlog_dir failed");
          perror("mkdir");
          return ERROR_STOP;
        }
      }

      if(like_reset_slave() == ERROR_STOP)
      {
        return ERROR_STOP;
      }

      break;
    }
    case 1:
    {
      //1:decide by "show master status"(clean binlog_dir);
      opt_remote_proto = BINLOG_DUMP_GTID;

      retry_connect_show_master_status:
      if (safe_connect() != OK_CONTINUE)
      {
        my_sleep(5000);
        sql_print_error("connect to master failed:%i,%s;reconnecting...",mysql_errno(mysql),mysql_error(mysql));
        goto retry_connect_show_master_status;
      }
      if(mysql_real_query(mysql,
                          "show global variables like 'gtid_executed'",
                          strlen("show global variables like 'gtid_executed'"))!=0)
      {
        sql_print_error("get master GTID Executed failed:%i,%s",mysql_errno(mysql),mysql_error(mysql));
        return ERROR_STOP;
      }
      MYSQL_RES* res = mysql_store_result(mysql);
      MYSQL_ROW row = mysql_fetch_row(res);
      if(row[1])
      {
        opt_exclude_gtids_str =strdup(row[1]);
      }
      else
      {
        opt_exclude_gtids_str = strdup("");
      }
      mysql_free_result(res);
      mysql_close(mysql);
      binlog_file_open_mode = O_WRONLY | O_BINARY;

      if(like_reset_slave() == ERROR_STOP)
      {
        return ERROR_STOP;
      }
      break;
    }

    case 2:
    {
      //2:decide by last file and pos in binlog_dir;appending
      opt_remote_proto = BINLOG_DUMP_NON_GTID;
      if(search_last_file_position() != OK_CONTINUE)
      {
        sql_print_error("Search last file position from index file failed");
        return ERROR_STOP;
      }

      break;
    }

  }
  if(set_gtid_executed() == ERROR_STOP)
  {
    DBUG_RETURN(ERROR_STOP);
  }

  DBUG_RETURN(OK_CONTINUE);
}

Exit_status get_executed_gtid()
{
  mysql_free_result(mysql_store_result(mysql));
  if(mysql_real_query(mysql,
                      "show global variables like 'gtid_executed'",
                      strlen("show global variables like 'gtid_executed'"))!=0)
  {
    sql_print_error("get master GTID Executed failed:%i,%s",mysql_errno(mysql),mysql_error(mysql));
    return ERROR_STOP;
  }
  MYSQL_RES* res = mysql_store_result(mysql);
  MYSQL_ROW row = mysql_fetch_row(res);
  if(row[1])
  {
    opt_exclude_gtids_str =strdup(row[1]);
  }
  else
  {
    opt_exclude_gtids_str = strdup("");
  }
  mysql_free_result(res);

  return OK_CONTINUE;
}


Exit_status set_gtid_executed()
{
  global_sid_lock->rdlock();

  if (opt_exclude_gtids_str != NULL)
  {
    if (gtid_set_excluded->add_gtid_text(opt_exclude_gtids_str) !=
        RETURN_STATUS_OK)
    {
      sql_print_error("Could not configure --exclude-gtids '%s'", opt_exclude_gtids_str);
      global_sid_lock->unlock();
      return (ERROR_STOP);
    }
  }

  global_sid_lock->unlock();
  return OK_CONTINUE;
}

/**
 * Get master uuid and set switch(true or false);
 * @return ERROR_STOP:failed; OK_CONTINUE:successfully;
 */
Exit_status get_master_uuid()
{
  char query[] ="show global variables like 'server_uuid'";

  mysql_free_result(mysql_store_result(mysql));
  if(mysql_real_query(mysql,
                      query,
                      strlen(query))!=0)
  {
    sql_print_error("get master uuid failed:%i,%s",mysql_errno(mysql),mysql_error(mysql));
    return ERROR_STOP;
  }
  MYSQL_RES* res = mysql_store_result(mysql);
  MYSQL_ROW row = mysql_fetch_row(res);
  if(row[1])
  {
     if(master_uuid_new)
     {
       free(master_uuid_new);
     }

     master_uuid_new=strdup(row[1]);
     if(!master_uuid) //get master uuid first times.
     {
       sql_print_information("connect to master first times,master_uuid:%s",master_uuid_new);
       master_uuid = strdup(master_uuid_new);
       switched=false;
     }
     else //get master uuid next times.
     {
       sql_print_information("reconnect to master,master_uuid:%s",master_uuid_new);
       if(strcasecmp(master_uuid,master_uuid_new) ==0 )
       {
         sql_print_information("M-S does not switch");
         switched=false;
       }
       else
       {
         sql_print_information("M-S switched");
         if(strlen(master_uuid_old))
         {
           free(master_uuid_old);
         }
         master_uuid_old = strdup(master_uuid);
         free(master_uuid);
         master_uuid = strdup(master_uuid_new);
         switched=true;
       }
     }
     free(master_uuid_new);
     master_uuid_new=0;
  }
  else
  {
    sql_print_error("get master uuid failed:%i,%s",mysql_errno(mysql),mysql_error(mysql));
    return ERROR_STOP;
  }
  mysql_free_result(res);

  return OK_CONTINUE;
}



/**
 * There are two conditions.
 * -- read binlog error,master does not change,
 * -- read binlog error,master was changed.
 */


/**
 *
 * @return
 */
Exit_status open_index_file()
{
  if(chdir(output_file))
  {
    sql_print_error("change work dir to %s failed",output_file);
    return ERROR_STOP;
  }
  sql_print_information("current work dir %s",getcwd(NULL,0));

  if(!index_file_name)
  {
    sql_print_error("index file is not setted");
  }

  if(!(binary_log_index_file = my_fopen(index_file_name, O_RDWR| FAPPEND,
                                        MYF(MY_WME))))
  {
    sql_print_error("Could not create log file '%s'", index_file_name);
    return ERROR_STOP;
  }
  return OK_CONTINUE;
}

/**
 * the func is invoked in these situation.
 * 1.start up
 * 2.binlog rotate
 * @return
 */
Exit_status purge_binlog_file()
{
  return OK_CONTINUE;
}

Exit_status search_last_file_position()
{
  fseek(binary_log_index_file,0,SEEK_END);
  char current_file[FN_REFLEN+1];
  // char last_log_file_name[FN_REFLEN+1];
  char event_buffer[50];
  //Log_event *ev= NULL;
  Log_event_type type;
//  const char *error_msg= NULL;
  unsigned long long last_pos=0;

  if(!ftell(binary_log_index_file)) //There is no binary logfile.change get_start_gtid_mode.
  {
    get_start_gtid_mode=1;
    return determine_dump_mode();
  }

  fseek(binary_log_index_file,0,SEEK_SET);
  glob_description_event= new Format_description_log_event(3);
  while(fgets(current_file,FN_REFLEN+1,binary_log_index_file)){}

  if(current_file[strlen(current_file)-1] == '\n')
  {
    current_file[strlen(current_file)-1] ='\0';
  }

  //Assume that the last binary log is net over.(has a complete xid event)
  FILE* last_file = my_fopen(current_file, O_RDWR|O_BINARY| FAPPEND,MYF(MY_WME));
  if(last_file)
  {
    fseek(last_file,-31,SEEK_END);
  }

  int read_len=my_fread(last_file,(uchar*)event_buffer,50,MYF(MY_WME));
  if(read_len == -1)
  {
    sql_print_error("read last binlog file error");
    return ERROR_STOP;
  }

  type = (Log_event_type)event_buffer[EVENT_TYPE_OFFSET];

  if(type ==  binary_log::XID_EVENT)
  {
    memcpy(&last_pos, event_buffer + LOG_POS_OFFSET, 4);
  }
  if(last_pos)
  {
    re_connect_start_position = last_pos;
    strcpy(new_binlog_file_name,current_file);
    binlog_file_open_mode = O_WRONLY | FAPPEND |O_BINARY ;
    recovery_mode =true;
    result_file = last_file;

  }
  else
  {
    //rewrite last binlog file.
    sql_print_warning("Could not find last pos in last file,overwrite the last binlog file %s",current_file);
    re_connect_start_position=4;
    strcpy(new_binlog_file_name,current_file);
    binlog_file_open_mode = O_WRONLY |O_BINARY ;
  }


  return OK_CONTINUE;
}

Exit_status like_reset_slave()
{
  fseek(binary_log_index_file,SEEK_SET,0);
  char current_file[FN_REFLEN+1];
  while(fgets(current_file,FN_REFLEN+1,binary_log_index_file))
  {
    if(current_file[strlen(current_file)-1] == '\n')
    {
      current_file[strlen(current_file)-1] = '\0';
    }
    if(remove(current_file) !=0 )
    {
      sql_print_warning("reset slave error remove file:%s",current_file);
    }
  }

  //clear index file
  ftruncate(fileno(binary_log_index_file),SEEK_SET);
  return OK_CONTINUE;
}


Exit_status prepare_log_file(char* log_file)
{
  if(chdir(output_file))
  {
    sql_print_error("change dir failed %s",output_file);
    return ERROR_STOP;
  }
  init_error_log(log_level);

  if (open_error_log(log_file))
  {
    sql_print_error("failed open log file: %s",log_file);
    return ERROR_STOP;
  }
  else
  {
    return OK_CONTINUE;
  }

}
