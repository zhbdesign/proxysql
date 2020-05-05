#include <algorithm>
#include <memory>
#include <numeric>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <type_traits>
#include <iostream>
#include <functional>

#include <mysql.h>
#include <mysql/mysqld_error.h>
#include <unistd.h>

#include "tap.h"
#include "command_line.h"
#include "utils.h"

using std::string;

int test_servers_tables(MYSQL* l_proxysql_admin) {
	// Initilize remote proxysql handler
	MYSQL* r_proxysql_admin = mysql_init(NULL);
	if (!r_proxysql_admin) {
		diag("%s", err_msg(mysql_error(r_proxysql_admin), __FILE__, __LINE__).c_str());
		return -1;
	}

	// Connnect to remote proxysql
	if (!mysql_real_connect(r_proxysql_admin, "127.0.0.1", "radmin", "radmin", NULL, 16032, NULL, 0)) {
		diag("%s", err_msg(mysql_error(r_proxysql_admin), __FILE__, __LINE__).c_str());
		return -1;
	}

	// Get docker default bridge interface
	const string docker_bridge_cmd =
		"ip -4 addr show docker0 | grep -oP '(?<=inet\\s)\\d+(\\.\\d+){3}'";
	string bridge_ip {};
	int exec_err = exec(docker_bridge_cmd, bridge_ip);
	if (exec_err) {
		diag("%s", err_msg("Failed to get default docker bridge interface.", __FILE__, __LINE__).c_str());
		return -1;
	}

	// Remove new line from `exec` result
	bridge_ip.erase(std::remove(bridge_ip.begin(), bridge_ip.end(), '\n'), bridge_ip.end());

	// Proxysql cluster config
	const char* clean_servers =
		"DELETE FROM proxysql_servers";
	const char* l_proxysql_config =
		"INSERT INTO proxysql_servers (hostname,port,weight,comment)"
		" VALUES ('127.0.0.1', 16032, 0, 'proxysql_replica')";
	const char* tr_proxysql_config =
		"INSERT INTO proxysql_servers (hostname,port,weight,comment)"
		" VALUES ('%s', 6032, 0, 'proxysql_master')";

	// Format the cluster config with default docker_bridge
	std::string r_proxysql_config {};
	int fmt_err = string_format(tr_proxysql_config, r_proxysql_config, bridge_ip.c_str());
	if (fmt_err) {
		diag("%s", err_msg("formatting error.", __FILE__, __LINE__).c_str());
		return -1;
	}

	// Clean current servers
	MYSQL_QUERY(l_proxysql_admin, clean_servers);
	MYSQL_QUERY(r_proxysql_admin, clean_servers);

	// Configure cluster nodes
	MYSQL_QUERY(l_proxysql_admin, l_proxysql_config);
	MYSQL_QUERY(r_proxysql_admin, r_proxysql_config.c_str());

	// Load servers to runtime to tag master node
	MYSQL_QUERY(l_proxysql_admin, "LOAD PROXYSQL SERVERS TO RUNTIME");
	MYSQL_QUERY(r_proxysql_admin, "LOAD PROXYSQL SERVERS TO RUNTIME");

	MYSQL_QUERY(l_proxysql_admin, "SELECT * FROM proxysql_servers");
	MYSQL_RES* result = mysql_store_result(l_proxysql_admin);

	// Check that runtime table have same values as config table
	// ========================================================================

	const char* compare_runtime_and_config =
		"SELECT COUNT(*) FROM proxysql_servers t1 "
		"NATURAL JOIN runtime_proxysql_servers t2";

	MYSQL_QUERY(l_proxysql_admin, compare_runtime_and_config);
	MYSQL_RES* cmp_res = mysql_store_result(l_proxysql_admin);
	int cmp_count = fetch_count(cmp_res);

	// Check runtime_proxysql_servers table format
	// ========================================================================
	ok(cmp_count == 1, "%s",
		err_msg("'proxysql_servers' and 'runtime_proxysql_servers' should be equal.", __FILE__, __LINE__).c_str());

	const char* check_l_server_config =
		"SELECT COUNT (*) FROM runtime_proxysql_servers WHERE "
		"hostname='127.0.0.1' AND port=16032 AND "
		"weight=0 AND comment='proxysql_replica'";

	MYSQL_QUERY(l_proxysql_admin, check_l_server_config);
	MYSQL_RES* check_res = mysql_store_result(l_proxysql_admin);
	int count = fetch_count(check_res);

	ok(count == 1, "%s",
		err_msg("'proxysql_servers' should contain inserted info.", __FILE__, __LINE__).c_str());

	// ========================================================================

	// Check stats_proxysql_servers_checksums table format
	// ========================================================================

	const char* check_server_stats =
		"SELECT COUNT (*) FROM stats_proxysql_servers_checksums WHERE "
		"hostname='127.0.0.1' AND port=16032 AND name='proxysql_servers' AND version!='' AND epoch!='' AND checksum!='' "
		"AND changed_at!='' AND updated_at!='' and diff_check!=''";
	MYSQL_QUERY(l_proxysql_admin, check_server_stats);
	MYSQL_RES* server_stats = mysql_store_result(l_proxysql_admin);
	int server_stats_count = fetch_count(server_stats);

	const std::string exp_row_err =
		"'proxysql_servers' row in 'stats_proxysql_servers_checksums' should not be empty.";
	ok(server_stats_count == 1, "%s", err_msg(exp_row_err, __FILE__, __LINE__).c_str());

	const char* check_stats_rows =
		"SELECT COUNT (*) FROM stats_proxysql_servers_checksums WHERE "
		"hostname='127.0.0.1' AND port=16032 AND version!='' AND epoch!=''"
		"AND changed_at!='' AND updated_at!='' and diff_check!=''";
	MYSQL_QUERY(l_proxysql_admin, check_stats_rows);
	MYSQL_RES* server_stats_rows = mysql_store_result(l_proxysql_admin);
	int stats_row_count = fetch_count(server_stats_rows);

	const std::string rows_msg_err =
		"'Rows in 'stats_proxysql_servers_checksums' should match the exp values.";
	ok(stats_row_count == 6, "%s", err_msg(rows_msg_err, __FILE__, __LINE__).c_str());

	return 0;
}

int launch_prepared_statement(MYSQL* mysql_server) {
	int res = 0;
	MYSQL_STMT* stmt = mysql_stmt_init(mysql_server);

	MYSQL_BIND bind[1];
	memset(bind, 0, sizeof(bind));

	int data = 1;
	my_bool is_null = 0;

	const char* stmt_query = "select * from sysbench.prep_stmt_test_table where test_elem = ?";
	if (mysql_stmt_prepare(stmt, stmt_query, std::strlen(stmt_query))) {
		res = mysql_stmt_errno(stmt);
		goto cleanup;
	}

	// Initialize bind parameter
	bind[0].buffer_type = MYSQL_TYPE_SHORT;
	bind[0].buffer = reinterpret_cast<char*>(&data);
	bind[0].is_null = reinterpret_cast<char*>(&is_null);
	bind[0].length = 0;

	if (mysql_stmt_bind_param(stmt, bind)) {
		res = mysql_stmt_errno(stmt);
		goto cleanup;
	}

	if (mysql_stmt_execute(stmt)) {
		res = mysql_stmt_errno(stmt);
		goto cleanup;
	}

cleanup:
	// Required for performing more queries
	mysql_stmt_store_result(stmt);
	mysql_stmt_close(stmt);

	return res;
}

int test_stats_prepared_statements(MYSQL* l_proxysql_admin) {
	int res = 0;
	MYSQL* mysql_server = mysql_init(NULL);

	// Connnect to mysql
	if (!mysql_real_connect(mysql_server, "127.0.0.1", "root", "root", NULL, 6033, NULL, 0)) {
		diag("%s", err_msg(mysql_error(mysql_server), __FILE__, __LINE__).c_str());
		return -1;
	}

	const char* drop_test_table = "DROP TABLE IF EXISTS sysbench.prep_stmt_test_table";
	const char* create_test_table =
		"CREATE TABLE sysbench.prep_stmt_test_table ( "
			"test_elem INT NOT NULL)"; 
	const char* insert_into_test_table =
		"INSERT INTO sysbench.prep_stmt_test_table "
			"( test_elem ) VALUES "
			"( 1 )";

	MYSQL_QUERY(mysql_server, drop_test_table);
	MYSQL_QUERY(mysql_server, create_test_table);
	MYSQL_QUERY(mysql_server, insert_into_test_table);

	int stmt_err = launch_prepared_statement(mysql_server);
	int timeout = 3;
	int count = timeout / 10;

	// TODO: This behavior is not supported
	// ================================
	// while (stmt_err == ER_NO_SUCH_TABLE && count < timeout) {
	// 	usleep(3*1000*1000 / 10);
	// 	stmt_err = launch_prepared_statement(mysql_server);
	// 	count += timeout / 10;
	// }

	// Reopen and close the connection
	while (stmt_err == ER_NO_SUCH_TABLE) {
		usleep(3*1000*1000 / 10);
		mysql_close(mysql_server);

		MYSQL* mysql_server = mysql_init(NULL);

		// Connnect to mysql
		if (!mysql_real_connect(mysql_server, "127.0.0.1", "root", "root", NULL, 6033, NULL, 0)) {
			diag("%s", err_msg(mysql_error(mysql_server), __FILE__, __LINE__).c_str());
			res = -1;
			goto cleanup;
		}

		stmt_err = launch_prepared_statement(mysql_server);
		count += timeout / 10;
	}

	if (stmt_err) {
		res = -1;
		goto cleanup;
	}

	{
		// TODO:
		// ================
		// ref_count_client should be '1' but it may be currently '0' due to previous connection problem

		// Check the table status
		const char* check_stats_prepared_statementes =
			"SELECT COUNT(*) FROM stats.stats_mysql_prepared_statements_info WHERE "
			"global_stmt_id=1 AND hostgroup=1 AND schemaname='information_schema' AND username='root' AND "
			"digest!='' AND ref_count_client!='' AND ref_count_server!='' AND "
			"query='select * from sysbench.prep_stmt_test_table where test_elem = ?' ";

		MYSQL_QUERY(l_proxysql_admin, check_stats_prepared_statementes);
		MYSQL_RES* stats_result = mysql_store_result(l_proxysql_admin);
		int prepared_stmt_count = fetch_count(stats_result);

		const std::string error =
			"'stats.stats_mysql_prepared_statements_info' should be populated and contain the exp values.";
		ok(prepared_stmt_count == 1, "%s", err_msg(error, __FILE__, __LINE__).c_str());
	}

cleanup:

	MYSQL_QUERY(mysql_server, drop_test_table);

	return res;
}

int test_replication_hostgroups(MYSQL* l_proxysql_admin) {
	const char* delete_replication_hostgroups =
		"DELETE FROM mysql_replication_hostgroups";
	const char* update_replication_hostgroups_query =
		"INSERT INTO mysql_replication_hostgroups (writer_hostgroup, reader_hostgroup, comment) "
		"VALUES (0, 1, \"reader_writer_test_hostgroup\")";
	const char* load_mysql_servers =
		"LOAD MYSQL SERVERS TO RUNTIME";

	MYSQL_QUERY(l_proxysql_admin, delete_replication_hostgroups);
	MYSQL_QUERY(l_proxysql_admin, update_replication_hostgroups_query);
	MYSQL_QUERY(l_proxysql_admin, load_mysql_servers);

	// Compare with runtime table
	const char* compare_runtime_and_config =
		"SELECT COUNT(*) FROM mysql_replication_hostgroups t1 "
		"NATURAL JOIN runtime_mysql_replication_hostgroups t2";

	MYSQL_QUERY(l_proxysql_admin, compare_runtime_and_config);
	MYSQL_RES* cmp_res = mysql_store_result(l_proxysql_admin);
	int cmp_count = fetch_count(cmp_res);

	ok(cmp_count == 1, "'mysql_replication_hostgroups' and 'runtime_mysql_replication_hostgroups' should be identical.");

	// Check the table format
	const char* check_replication_hostgroups =
		"SELECT COUNT(*) FROM runtime_mysql_replication_hostgroups WHERE "
		"writer_hostgroup=0 AND reader_hostgroup=1 AND comment='reader_writer_test_hostgroup'";

	MYSQL_QUERY(l_proxysql_admin, check_replication_hostgroups);
	MYSQL_RES* check_rep_result = mysql_store_result(l_proxysql_admin);
	int rep_count = fetch_count(check_rep_result);

	ok(rep_count == 1, "'mysql_replication_hostgroups' should have the exp values.");

	MYSQL_QUERY(l_proxysql_admin, delete_replication_hostgroups);
	MYSQL_QUERY(l_proxysql_admin, load_mysql_servers);

	return 0;
}

int test_group_replication_hostgroups(MYSQL* l_proxysql_admin) {
	const char* delete_replication_hostgroups =
		"DELETE FROM mysql_group_replication_hostgroups";
	const char* update_replication_hostgroups_query =
		"INSERT INTO mysql_group_replication_hostgroups "
		"( writer_hostgroup, backup_writer_hostgroup, reader_hostgroup, offline_hostgroup, active, "
		"max_writers, writer_is_also_reader, max_transactions_behind, comment) "
		"VALUES (0, 1, 2, 3, 1, 10, 0, 200, \"reader_writer_test_group_hostgroup\")";
	const char* load_mysql_servers =
		"LOAD MYSQL SERVERS TO RUNTIME";

	MYSQL_QUERY(l_proxysql_admin, delete_replication_hostgroups);
	MYSQL_QUERY(l_proxysql_admin, update_replication_hostgroups_query);
	MYSQL_QUERY(l_proxysql_admin, load_mysql_servers);

	// Compare with runtime table
	const char* compare_runtime_and_config =
		"SELECT COUNT(*) FROM mysql_group_replication_hostgroups t1 "
		"NATURAL JOIN runtime_mysql_group_replication_hostgroups t2";

	MYSQL_QUERY(l_proxysql_admin, compare_runtime_and_config);
	MYSQL_RES* cmp_res = mysql_store_result(l_proxysql_admin);
	int cmp_count = fetch_count(cmp_res);

	ok(cmp_count == 1,
		"'mysql_group_replication_hostgroups' and 'runtime_mysql_group_replication_hostgroups' should be identical.");

	MYSQL_QUERY(l_proxysql_admin, delete_replication_hostgroups);
	MYSQL_QUERY(l_proxysql_admin, load_mysql_servers);

	return 0;
}

int test_free_connections_stats(MYSQL* l_proxysql_admin) {
	const char* check_mysql_info_consistency =
		"SELECT COUNT(*) FROM stats_mysql_free_connections WHERE "
		"srv_host = JSON_EXTRACT(mysql_info,'$.host') AND "
		"srv_port = JSON_EXTRACT(mysql_info,'$.port') AND "
		"JSON_EXTRACT(statistics,'$.questions') > 0";

	MYSQL_QUERY(l_proxysql_admin, check_mysql_info_consistency);
	MYSQL_RES* info_res = mysql_store_result(l_proxysql_admin);
	int info_count = fetch_count(info_res);

	ok(info_count == 1, "%s",
		err_msg("'stats_mysql_free_connections' fields should be consistent with 'mysql_info'.",
		__FILE__,
		__LINE__).c_str()
	);

	return 0;
}

using test_data = std::pair<std::string, std::function<int(MYSQL*)>>;

const std::vector<test_data> table_tests {
	test_data ( "admin_config_tests: Check the servers tables.", test_servers_tables ),
	test_data ( "admin_config_tests: Check the 'stats_mysql_prepared_statements_info' table.", test_stats_prepared_statements),
	test_data ( "admin_config_tests: Check the 'mysql_replication_hostgroups' table.", test_replication_hostgroups ),
	test_data ( "admin_config_tests: Check the 'mysql_group_replication_hostgroups' table.", test_group_replication_hostgroups ),
	test_data ( "admin_config_tests: Check the 'free_connections_stats' table.", test_free_connections_stats )
};

int main(int argc, char** argv) {
	CommandLine cl;
	
	if (cl.getEnv()) {
		diag("Failed to get the required environmental variables.");
		return -1;
	}

	plan(5);

	diag("admin_config_tests: Initialize common resources.");

	MYSQL* l_proxysql_admin = mysql_init(NULL);

	// Initialize connections
	if (!l_proxysql_admin) {
		diag("%s", err_msg(mysql_error(l_proxysql_admin), __FILE__, __LINE__).c_str());
		return -1;
	}

	// Connnect to local proxysql
	if (!mysql_real_connect(l_proxysql_admin, "127.0.0.1", "admin", "admin", NULL, 6032, NULL, 0)) {
		diag("%s", err_msg(mysql_error(l_proxysql_admin), __FILE__, __LINE__).c_str());
		return -1;
	}

	// Execute all the defined tests
	for (const auto& test : table_tests) {
		diag("%s", test.first.c_str());
		int test_res = test.second(l_proxysql_admin);

		if (test_res) {
			return exit_status();
		}
	}
}