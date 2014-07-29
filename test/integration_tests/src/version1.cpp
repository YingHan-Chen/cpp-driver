#define BOOST_TEST_DYN_LINK
#ifdef STAND_ALONE
#   define BOOST_TEST_MODULE cassandra
#endif

#include <algorithm>

#include <boost/test/unit_test.hpp>
#include <boost/test/debug.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/cstdint.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/chrono.hpp>

#include "cassandra.h"
#include "test_utils.hpp"

const CassPrepared* prepare_statement(CassSession* session, std::string query) {
  test_utils::CassFuturePtr prepared_future = test_utils::make_shared(cass_session_prepare(session,
                                                                        cass_string_init2(query.data(), query.size())));
  test_utils::wait_and_check_error(prepared_future.get());
  return cass_future_get_prepared(prepared_future.get());
}

void check_result(CassSession* session) {
  test_utils::CassResultPtr result;
  test_utils::execute_query(session, "SELECT * FROM test", &result);
  BOOST_REQUIRE(cass_result_column_count(result.get()) == 5);
  BOOST_REQUIRE(cass_result_row_count(result.get()) > 0);

  const CassRow* row = cass_result_first_row(result.get());

  cass_int32_t key;
  BOOST_REQUIRE(cass_value_get_int32(cass_row_get_column(row, 0), &key) == CASS_OK);

  cass_int32_t v1;
  BOOST_REQUIRE(cass_value_get_int32(cass_row_get_column(row, 1), &v1) == CASS_OK);

  CassString v2;
  BOOST_REQUIRE(cass_value_get_string(cass_row_get_column(row, 2), &v2) == CASS_OK);

  test_utils::CassIteratorPtr v3 = test_utils::make_shared(cass_iterator_from_collection(cass_row_get_column(row, 3)));

  cass_int32_t i = 0;
  while (cass_iterator_next(v3.get())) {
    const CassValue* value =  cass_iterator_get_value(v3.get());
    BOOST_REQUIRE(cass_value_type(value) == CASS_VALUE_TYPE_INT);

    cass_int32_t output;
    cass_value_get_int32(value, &output);
    BOOST_REQUIRE(i == output);
    i++;
  }

  test_utils::CassIteratorPtr v4 = test_utils::make_shared(cass_iterator_from_collection(cass_row_get_column(row, 4)));

  cass_int32_t j = 0;
  while (cass_iterator_next(v4.get())) {
    const CassValue* value =  cass_iterator_get_value(v4.get());
    BOOST_REQUIRE(cass_value_type(value) == CASS_VALUE_TYPE_VARCHAR);

    CassString output;
    cass_value_get_string(value, &output);
    BOOST_REQUIRE(output.length == 1 && output.data[0] == 'd' + j);
    j++;
  }
}

struct Version1Tests : public test_utils::SingleSessionTest {
    Version1Tests() : SingleSessionTest(1, 0, 1) { // v1
      test_utils::execute_query(session, str(boost::format(test_utils::CREATE_KEYSPACE_SIMPLE_FORMAT)
                                             % test_utils::SIMPLE_KEYSPACE % "1"));
      test_utils::execute_query(session, str(boost::format("USE %s") % test_utils::SIMPLE_KEYSPACE));
    }
};

BOOST_FIXTURE_TEST_SUITE(version1, Version1Tests)

BOOST_AUTO_TEST_CASE(test_query)
{
  test_utils::execute_query(session, "CREATE TABLE test (key int PRIMARY KEY, v1 int, v2 text, v3 list<int>, v4 set<text>);");
  test_utils::execute_query(session, "INSERT INTO test (key, v1, v2, v3, v4) VALUES (0, 99, 'abc', [ 0, 1, 2 ], { 'd', 'e', 'f' });");
  check_result(session);
}

BOOST_AUTO_TEST_CASE(test_prepared)
{
  test_utils::execute_query(session, "CREATE TABLE test (key int PRIMARY KEY, v1 int, v2 text, v3 list<int>, v4 set<text>);");

  test_utils::CassPreparedPtr prepared
      = test_utils::make_shared(
          prepare_statement(session, "INSERT INTO test (key, v1, v2, v3, v4) VALUES (?, ?, ?, ?, ?)"));

  test_utils::CassStatementPtr statement = test_utils::make_shared(cass_prepared_bind(prepared.get()));

  cass_statement_bind_int32(statement.get(), 0, 0);
  cass_statement_bind_int32(statement.get(), 1, 99);
  cass_statement_bind_string(statement.get(), 2, cass_string_init("abc"));

  test_utils::CassCollectionPtr list = test_utils::make_shared(cass_collection_new(CASS_COLLECTION_TYPE_LIST, 3));
  cass_collection_append_int32(list.get(), 0);
  cass_collection_append_int32(list.get(), 1);
  cass_collection_append_int32(list.get(), 2);
  cass_statement_bind_collection(statement.get(), 3, list.get());

  test_utils::CassCollectionPtr set = test_utils::make_shared(cass_collection_new(CASS_COLLECTION_TYPE_SET, 3));
  cass_collection_append_string(set.get(), cass_string_init("d"));
  cass_collection_append_string(set.get(), cass_string_init("e"));
  cass_collection_append_string(set.get(), cass_string_init("f"));
  cass_statement_bind_collection(statement.get(), 4, set.get());

  test_utils::CassFuturePtr future = test_utils::make_shared(cass_session_execute(session, statement.get()));
  test_utils::wait_and_check_error(future.get());

  check_result(session);
}

BOOST_AUTO_TEST_CASE(test_batch_error)
{
  test_utils::execute_query(session, "CREATE TABLE test (key int PRIMARY KEY, value int);");

  test_utils::CassBatchPtr batch = test_utils::make_shared(cass_batch_new(CASS_BATCH_TYPE_LOGGED));

  for(int x = 0; x < 4; x++)
  {
    std::string insert_query = str(boost::format("INSERT INTO test (key, value) VALUES(%d, %d);") % x % x);
    test_utils::CassStatementPtr insert_statement = test_utils::make_shared(cass_statement_new(cass_string_init(insert_query.c_str()), 0));
    cass_batch_add_statement(batch.get(), insert_statement.get());
  }

  test_utils::CassFuturePtr future = test_utils::make_shared(cass_session_execute_batch(session, batch.get()));

  CassError code = cass_future_error_code(future.get());
  CassString message = cass_future_error_message(future.get());
  BOOST_REQUIRE(code == CASS_ERROR_LIB_MESSAGE_ENCODE);
  BOOST_REQUIRE(std::string(message.data, message.length).find("Operation unsupported by this protocol version") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_query_param_error)
{
  test_utils::execute_query(session, "CREATE TABLE test (key int PRIMARY KEY, value int);");

  CassString insert_query = cass_string_init("INSERT INTO test (key, value) VALUES(?, ?);");

  test_utils::CassStatementPtr statement = test_utils::make_shared(cass_statement_new(insert_query, 2));

  cass_statement_bind_int32(statement.get(), 0, 11);
  cass_statement_bind_int32(statement.get(), 1, 99);

  test_utils::CassFuturePtr future = test_utils::make_shared(cass_session_execute(session, statement.get()));

  CassError code = cass_future_error_code(future.get());
  CassString message = cass_future_error_message(future.get());
  BOOST_REQUIRE(code == CASS_ERROR_SERVER_INVALID_QUERY);
  BOOST_REQUIRE(std::string(message.data, message.length).find("Invalid amount of bind variables") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
