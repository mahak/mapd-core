/*
 * Copyright 2022 HEAVY.AI, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.mapd.parser.server.test;

import static org.junit.Assert.*;

import com.mapd.common.SockTransportProperties;
import com.mapd.parser.server.CalciteServerWrapper;

import org.apache.thrift.TException;
import org.apache.thrift.protocol.TBinaryProtocol;
import org.apache.thrift.protocol.TProtocol;
import org.apache.thrift.transport.TSocket;
import org.apache.thrift.transport.TTransport;
import org.junit.AfterClass;
import org.junit.BeforeClass;
import org.junit.Ignore;
import org.junit.Test;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.ArrayList;
import java.util.Random;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import ai.heavy.thrift.calciteserver.CalciteServer;
import ai.heavy.thrift.calciteserver.TOptimizationOption;
import ai.heavy.thrift.calciteserver.TPlanResult;
import ai.heavy.thrift.calciteserver.TQueryParsingOption;

public class TestServer {
  private final static Logger HEAVYDBLOGGER = LoggerFactory.getLogger(TestServer.class);
  private final static int TEST_THREAD_COUNT = 3;
  private volatile int threadsRun = 0;
  private volatile boolean threadHadFailure = false;
  private volatile AssertionError ae;
  private static CalciteServerWrapper csw = null;
  private static SockTransportProperties client_skT = null;
  private static SockTransportProperties server_skT = null;

  @BeforeClass
  public static void startServer() {
    csw = new CalciteServerWrapper(11000, 11001, "/data", null, client_skT, server_skT);
    new Thread(csw).start();
  }

  @AfterClass
  public static void stopServer() {
    csw.stopServer();
  }

  @Ignore
  public void testThreadedCall() {
    final ExecutorService pool = Executors.newFixedThreadPool(TEST_THREAD_COUNT);

    Runnable r = new Runnable() {
      @Override
      public void run() {
        try {
          for (int i = 1; i <= 5; i++) {
            randomCalciteCall();
          }
        } catch (AssertionError x) {
          HEAVYDBLOGGER.error("error during Runnable");
          threadHadFailure = true;
          ae = x;
        }
        threadsRun++;
        if (threadsRun >= TEST_THREAD_COUNT) {
          pool.shutdown();
        }
      }
    };

    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
      pool.submit(r);
    }
    while (!pool.isShutdown()) {
      // stay alive
    }
    if (threadHadFailure) {
      throw ae;
    }
  }

  @Ignore
  public void testSimpleCall() {
    callCalciteCheck("Select ENAME from EMP",
            "{\n"
                    + "  \"rels\": [\n"
                    + "    {\n"
                    + "      \"id\": \"0\",\n"
                    + "      \"relOp\": \"LogicalTableScan\",\n"
                    + "      \"fieldNames\": [\n"
                    + "        \"EMPNO\",\n"
                    + "        \"ENAME\",\n"
                    + "        \"JOB\",\n"
                    + "        \"MGR\",\n"
                    + "        \"HIREDATE\",\n"
                    + "        \"SAL\",\n"
                    + "        \"COMM\",\n"
                    + "        \"DEPTNO\",\n"
                    + "        \"SLACKER\",\n"
                    + "        \"SLACKARR1\",\n"
                    + "        \"SLACKARR2\"\n"
                    + "      ],\n"
                    + "      \"table\": [\n"
                    + "        \"CATALOG\",\n"
                    + "        \"SALES\",\n"
                    + "        \"EMP\"\n"
                    + "      ],\n"
                    + "      \"inputs\": []\n"
                    + "    },\n"
                    + "    {\n"
                    + "      \"id\": \"1\",\n"
                    + "      \"relOp\": \"LogicalProject\",\n"
                    + "      \"fields\": [\n"
                    + "        \"ENAME\"\n"
                    + "      ],\n"
                    + "      \"exprs\": [\n"
                    + "        {\n"
                    + "          \"input\": 1\n"
                    + "        }\n"
                    + "      ]\n"
                    + "    }\n"
                    + "  ]\n"
                    + "}");
  }

  @Ignore
  public void testRandomCall() {
    randomCalciteCall();
  }

  private void randomCalciteCall() {
    Random r = new Random();
    int aliasID = r.nextInt(100000) + 1000000;
    callCalciteCheck(
            String.format(
                    "Select TABALIAS%d.ENAME AS COLALIAS%d from EMP TABALIAS%d LIMIT %d",
                    aliasID,
                    aliasID,
                    aliasID,
                    aliasID),
            String.format("{\n"
                            + "  \"rels\": [\n"
                            + "    {\n"
                            + "      \"id\": \"0\",\n"
                            + "      \"relOp\": \"LogicalTableScan\",\n"
                            + "      \"fieldNames\": [\n"
                            + "        \"EMPNO\",\n"
                            + "        \"ENAME\",\n"
                            + "        \"JOB\",\n"
                            + "        \"MGR\",\n"
                            + "        \"HIREDATE\",\n"
                            + "        \"SAL\",\n"
                            + "        \"COMM\",\n"
                            + "        \"DEPTNO\",\n"
                            + "        \"SLACKER\",\n"
                            + "        \"SLACKARR1\",\n"
                            + "        \"SLACKARR2\"\n"
                            + "      ],\n"
                            + "      \"table\": [\n"
                            + "        \"CATALOG\",\n"
                            + "        \"SALES\",\n"
                            + "        \"EMP\"\n"
                            + "      ],\n"
                            + "      \"inputs\": []\n"
                            + "    },\n"
                            + "    {\n"
                            + "      \"id\": \"1\",\n"
                            + "      \"relOp\": \"LogicalProject\",\n"
                            + "      \"fields\": [\n"
                            + "        \"COLALIAS%d\"\n"
                            + "      ],\n"
                            + "      \"exprs\": [\n"
                            + "        {\n"
                            + "          \"input\": 1\n"
                            + "        }\n"
                            + "      ]\n"
                            + "    },\n"
                            + "    {\n"
                            + "      \"id\": \"2\",\n"
                            + "      \"relOp\": \"LogicalSort\",\n"
                            + "      \"collation\": [],\n"
                            + "      \"fetch\": {\n"
                            + "        \"literal\": %d,\n"
                            + "        \"type\": \"DECIMAL\",\n"
                            + "        \"scale\": 0,\n"
                            + "        \"precision\": 7,\n"
                            + "        \"type_scale\": 0,\n"
                            + "        \"type_precision\": 10\n"
                            + "      }\n"
                            + "    },\n"
                            + "    {\n"
                            + "      \"id\": \"3\",\n"
                            + "      \"relOp\": \"LogicalProject\",\n"
                            + "      \"fields\": [\n"
                            + "        \"COLALIAS%d\"\n"
                            + "      ],\n"
                            + "      \"exprs\": [\n"
                            + "        {\n"
                            + "          \"input\": 0\n"
                            + "        }\n"
                            + "      ]\n"
                            + "    },\n"
                            + "    {\n"
                            + "      \"id\": \"4\",\n"
                            + "      \"relOp\": \"LogicalSort\",\n"
                            + "      \"collation\": [],\n"
                            + "      \"fetch\": {\n"
                            + "        \"literal\": %d,\n"
                            + "        \"type\": \"DECIMAL\",\n"
                            + "        \"scale\": 0,\n"
                            + "        \"precision\": 7,\n"
                            + "        \"type_scale\": 0,\n"
                            + "        \"type_precision\": 10\n"
                            + "      }\n"
                            + "    }\n"
                            + "  ]\n"
                            + "}",
                    aliasID,
                    aliasID,
                    aliasID,
                    aliasID));
  }

  private void callCalciteCheck(String query, String result) {
    try {
      TTransport transport;
      transport = new TSocket("localhost", 11000);
      transport.open();
      TProtocol protocol = new TBinaryProtocol(transport);
      CalciteServer.Client client = new CalciteServer.Client(protocol);

      TQueryParsingOption queryParsingOption = new TQueryParsingOption();
      queryParsingOption.legacy_syntax = false;
      queryParsingOption.is_explain = false;
      queryParsingOption.is_explain_detail = false;

      TOptimizationOption optimizationOption = new TOptimizationOption();
      optimizationOption.is_view_optimize = false;
      optimizationOption.enable_watchdog = false;
      optimizationOption.filter_push_down_info = new ArrayList<>();

      TPlanResult algebra = client.process("user",
              "passwd",
              "SALES",
              query,
              queryParsingOption,
              optimizationOption,
              null);
      transport.close();
      try {
        assertEquals(algebra.plan_result, result);
      } catch (AssertionError s) {
        HEAVYDBLOGGER.error("error during callCalciteCheck");
        throw s;
      }
    } catch (TException x) {
      fail(x.toString());
    }
  }
}
