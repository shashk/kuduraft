// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <boost/optional/optional.hpp>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/client/client-internal.h"
#include "kudu/client/client-test-util.h"
#include "kudu/client/client.h"
#include "kudu/client/row_result.h"
#include "kudu/client/schema.h"
#include "kudu/client/shared_ptr.h"
#include "kudu/client/write_op.h"
#include "kudu/common/partial_row.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/walltime.h"
#include "kudu/integration-tests/test_workload.h"
#include "kudu/master/master.h"
#include "kudu/master/mini_master.h"
#include "kudu/mini-cluster/internal_mini_cluster.h"
#include "kudu/rpc/messenger.h"
#include "kudu/security/crypto.h"
#include "kudu/security/openssl_util.h"
#include "kudu/security/token.pb.h"
#include "kudu/security/token_signer.h"
#include "kudu/security/token_verifier.h"
#include "kudu/tablet/key_value_test_schema.h"
#include "kudu/util/monotime.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

DECLARE_bool(rpc_reopen_outbound_connections);
DECLARE_int32(heartbeat_interval_ms);

using std::atomic;
using std::string;
using std::thread;
using std::unique_ptr;

namespace kudu {

using client::KuduClient;
using client::KuduClientBuilder;
using client::KuduError;
using client::KuduInsert;
using client::KuduRowResult;
using client::KuduScanner;
using client::KuduSchema;
using client::KuduSession;
using client::KuduTable;
using client::KuduTableCreator;
using client::sp::shared_ptr;
using cluster::InternalMiniCluster;
using cluster::InternalMiniClusterOptions;
using security::DataFormat;
using security::PrivateKey;
using security::SignedTokenPB;
using security::TokenPB;
using security::TokenSigner;
using security::TokenSigningPrivateKeyPB;
using security::TokenVerifier;
using security::VerificationResult;

class SecurityUnknownTskTest : public KuduTest {
 public:
  SecurityUnknownTskTest()
      : num_tablet_servers_(3),
        heartbeat_interval_ms_(100),
        schema_(client::KuduSchemaFromSchema(CreateKeyValueTestSchema())) {
    // Make the ts->master heartbeat interval shorter to run the test faster.
    FLAGS_heartbeat_interval_ms = heartbeat_interval_ms_;

    // Within the scope of the same reactor thread, close an already established
    // idle connection to the server and open a new one upon making another call
    // to the same server. This is to force authn token verification at each RPC
    // call: the authn token is verified by the server side during connection
    // negotiation. This test uses the in-process InternalMiniCluster, this
    // affects Kudu clients and the server components. In the context of this
    // test, that's crucial only for the Kudu clients used in the tests.
    FLAGS_rpc_reopen_outbound_connections = true;
  }

  void SetUp() override {
    KuduTest::SetUp();

    InternalMiniClusterOptions opts;
    opts.num_tablet_servers = num_tablet_servers_;
    cluster_.reset(new InternalMiniCluster(env_, opts));
    ASSERT_OK(cluster_->Start());
  }

  void TearDown() override {
    cluster_->Shutdown();
  }

  // Generate custom TSK.
  Status GenerateTsk(TokenSigningPrivateKeyPB* tsk, int64_t seq_num = 100) {
    PrivateKey private_key;
    RETURN_NOT_OK(GeneratePrivateKey(512, &private_key));
    string private_key_str_der;
    RETURN_NOT_OK(private_key.ToString(&private_key_str_der, DataFormat::DER));
    tsk->set_rsa_key_der(private_key_str_der);
    // Key sequence number should be high enough to be greater than sequence
    // numbers of the TSKs generated by the master itself.
    tsk->set_key_seq_num(seq_num);
    tsk->set_expire_unix_epoch_seconds(WallTime_Now() + 3600);
    return Status::OK();
  }

  // Generate authn token signed by the specified TSK. Use current client's
  // authn token as a 'template' for the new one signed by the specified TSK.
  Status GenerateAuthnToken(
      const shared_ptr<KuduClient>& client,
      const TokenSigningPrivateKeyPB& tsk,
      SignedTokenPB* new_signed_token) {
    // Should be already connected to the cluster.
    boost::optional<SignedTokenPB> authn_token =
        client->data_->messenger_->authn_token();
    if (authn_token == boost::none) {
      return Status::RuntimeError("client authn token is not set");
    }

    // 'Copy' the token data. The idea to is remove the signature from the
    // signed token and sign the token with our custom TSK (see below).
    TokenSigner* signer = cluster_->mini_master()->master()->token_signer();
    TokenPB token;
    const TokenVerifier& verifier = signer->verifier();
    if (verifier.VerifyTokenSignature(*authn_token, &token) !=
        VerificationResult::VALID) {
      return Status::RuntimeError("current client authn token is not valid");
    }

    // Create an authn token, signing it with the custom TSK.
    if (!token.SerializeToString(new_signed_token->mutable_token_data())) {
      return Status::RuntimeError("failed to serialize token data");
    }

    TokenSigner forger(1, 1);
    RETURN_NOT_OK(forger.ImportKeys({tsk}));
    return forger.SignToken(new_signed_token);
  }

  // Replace client's authn token with the specified one.
  void ReplaceAuthnToken(KuduClient* client, const SignedTokenPB& token) {
    client->data_->messenger_->set_authn_token(token);
  }

  // Import the specified TSK into the master's TokenSigner. Once the TSK is
  // imported, the master is able to verify authn tokens signed by the TSK.
  // The tablet servers are able to verify corresponding authn tokens after
  // they receive TSK from the master with tserver-->master heartbeat response.
  Status ImportTsk(const TokenSigningPrivateKeyPB& tsk) {
    TokenSigner* signer = cluster_->mini_master()->master()->token_signer();
    return signer->ImportKeys({tsk});
  }

 protected:
  const int num_tablet_servers_;
  const int32_t heartbeat_interval_ms_;
  const KuduSchema schema_;
  unique_ptr<InternalMiniCluster> cluster_;
};

// Tablet server sends back ERROR_UNAVAILABLE error code upon connection
// negotiation if it does not recognize the TSK which the client's authn token
// is signed with. The client should receive ServiceUnavailable status in that
// case and retry the operation. This test exercises some common subset of
// client-->master and client-->tserver RPCs. The test verifies both success
// and failure scenarios for the selected subset of RPCs.
TEST_F(SecurityUnknownTskTest, ErrorUnavailableCommonOperations) {
  const string table_name = "security-unknown-tsk-itest";
  const int64_t timeout_seconds = 3;

  shared_ptr<KuduClient> client;
  ASSERT_OK(cluster_->CreateClient(
      &KuduClientBuilder()
           .default_admin_operation_timeout(
               MonoDelta::FromSeconds(timeout_seconds))
           .default_rpc_timeout(MonoDelta::FromSeconds(timeout_seconds)),
      &client));

  // Generate our custom TSK.
  TokenSigningPrivateKeyPB tsk;
  ASSERT_OK(GenerateTsk(&tsk));

  // Create new authn token, signing it with the custom TSK.
  SignedTokenPB new_signed_token;
  ASSERT_OK(GenerateAuthnToken(client, tsk, &new_signed_token));

  // Create and open a table: a proper table handle is necessary for further RPC
  // calls to the tablet server. The table should consists of multiple tablets
  // hosted by all available tablet servers, so the insert or scan requests are
  // sent to all avaialble tablet servers.
  gscoped_ptr<KuduTableCreator> table_creator(client->NewTableCreator());
  shared_ptr<KuduTable> table;
  ASSERT_OK(table_creator->table_name(table_name)
                .set_range_partition_columns({"key"})
                .add_hash_partitions({"key"}, num_tablet_servers_)
                .schema(&schema_)
                .num_replicas(1)
                .Create());
  ASSERT_OK(client->OpenTable(table_name, &table));

  shared_ptr<KuduSession> session = client->NewSession();
  // We want to send the write batch to the server as soon as it's applied.
  ASSERT_OK(session->SetFlushMode(KuduSession::AUTO_FLUSH_SYNC));

  // Insert a row into the table -- this is to populate the client's metadata
  // cache so the client wont try to do that later while trying to re-insert the
  // same data. If not doing that, then the Apply() call for the duplicate
  // insert would not be sent to the tablet server: instead, it would be sent to
  // the master server to find about location of the target tablet. The idea is
  // to cover client-->tserver RPCs by this test as well.
  {
    unique_ptr<KuduInsert> ins(table->NewInsert());
    ASSERT_OK(ins->mutable_row()->SetInt32(0, -1));
    ASSERT_OK(ins->mutable_row()->SetInt32(1, -1));
    ASSERT_OK(session->Apply(ins.release()));
  }

  // Replace the original authn token with the specially crafted one. From this
  // point until importing the custom TSK into the master's verifier, the master
  // and tablet servers should respond with ERROR_UNAVAILABLE because the client
  // is about to present authn token signed with unknown TSK.
  ReplaceAuthnToken(client.get(), new_signed_token);

  // Try to create the table again: this time the RPC shall not pass since the
  // authn token has been replaced (but not because the table already exists).
  {
    const Status s = table_creator->table_name(table_name)
                         .set_range_partition_columns({"key"})
                         .schema(&schema_)
                         .num_replicas(1)
                         .Create();
    // The client automatically retries on getting ServiceUnavailable from the
    // master. It will retry in vain until the operation times out.
    const string err_msg = s.ToString();
    ASSERT_TRUE(s.IsTimedOut()) << err_msg;
    ASSERT_STR_CONTAINS(
        err_msg, "CreateTable timed out after deadline expired");
  }

  {
    shared_ptr<KuduTable> table;
    const Status s = client->OpenTable(table_name, &table);
    const string err_msg = s.ToString();
    ASSERT_TRUE(s.IsTimedOut()) << err_msg;
    ASSERT_STR_CONTAINS(
        err_msg, "GetTableSchema timed out after deadline expired");
  }

  // Try to insert the same data which has been successfully inserted prior to
  // replacing the authn token. The idea is to exercise client-->tablet server
  // path: the meta-cache already contains information on the corresponding
  // tablet server and the client will try to send RPCs to the tablet server
  // directly, avoiding calls to the master server which would happen if the
  // meta-cache did not contain the information on the tablet location.
  {
    unique_ptr<KuduInsert> ins(table->NewInsert());
    ASSERT_OK(ins->mutable_row()->SetInt32(0, -1));
    ASSERT_OK(ins->mutable_row()->SetInt32(1, -1));
    const Status s_apply = session->Apply(ins.release());
    // The error returned is a generic IOError, and the details are provided
    // by the KuduSession::GetPendingErrors() method.
    ASSERT_TRUE(s_apply.IsIOError()) << s_apply.ToString();
    ASSERT_STR_CONTAINS(s_apply.ToString(), "Some errors occurred");

    std::vector<KuduError*> errors;
    ElementDeleter cleanup(&errors);

    session->GetPendingErrors(&errors, nullptr);
    ASSERT_EQ(1, errors.size());
    ASSERT_NE(nullptr, errors[0]);
    const Status& ps = errors[0]->status();
    const string err_msg = ps.ToString();
    // The client automatically retries on getting ServiceUnavailable from the
    // tablet server. It will retry in vain until the operation times out.
    ASSERT_TRUE(ps.IsTimedOut()) << err_msg;
    ASSERT_STR_CONTAINS(err_msg, "Failed to write batch of 1 ops");
  }

  // Try opening a scanner. This should fail, timing out on retries.
  {
    KuduScanner scanner(table.get());
    ASSERT_OK(scanner.SetSelection(KuduClient::LEADER_ONLY));
    ASSERT_OK(scanner.SetTimeoutMillis(1000));
    const Status s = scanner.Open();
    const string err_msg = s.ToString();
    ASSERT_TRUE(s.IsTimedOut()) << err_msg;
    ASSERT_STR_CONTAINS(err_msg, "GetTableLocations");
  }

  // In a separate thread, import our TSK into the master's TokenSigner. After
  // importing, the TSK should propagate to the tablet servers and the client
  // should be able to authenticate using its custom authn token.
  thread importer([&]() {
    SleepFor(MonoDelta::FromMilliseconds(timeout_seconds * 1000 / 5));
    CHECK_OK(ImportTsk(tsk));
  });

  // An automatic clean-up to handle failure cases in the code below.
  SCOPED_CLEANUP({ importer.join(); });

  // The client should retry its operations until the masters and tablet servers
  // get the necessary token verification key to verify our custom authn token.
  for (int i = 0; i < num_tablet_servers_; ++i) {
    unique_ptr<KuduInsert> ins(table->NewInsert());
    ASSERT_OK(ins->mutable_row()->SetInt32(0, i));
    ASSERT_OK(ins->mutable_row()->SetInt32(1, i));
    ASSERT_OK(session->Apply(ins.release()));
  }

  // Run a scan to verify the number of inserted rows.
  EXPECT_EQ(num_tablet_servers_ + 1, client::CountTableRows(table.get()));
}

// Replace client's authn token while running a workload which includes creating
// a table, inserting data and reading it back. With huge number of runs,
// this gives coverage of ERROR_UNAVAILABLE handling for all RPC calls involved
// in the workload scenario.
TEST_F(SecurityUnknownTskTest, ErrorUnavailableDuringWorkload) {
  if (!AllowSlowTests()) {
    LOG(WARNING) << "test is skipped; set KUDU_ALLOW_SLOW_TESTS=1 to run";
    return;
  }

  static const int64_t kTimeoutMs = 20 * 1000;
  int64_t tsk_seq_num = 100;
  // Targeting the total runtime to be less than 3 minutes, and in most cases
  // less than 2 minutes. Sometimes a cycle might take two and in rare cases
  // up to three timeout intervals to complete.
  for (int i = 0; i < 3; ++i) {
    TestWorkload w(cluster_.get());
    w.set_num_tablets(num_tablet_servers_);
    w.set_num_replicas(1);
    w.set_num_read_threads(2);
    w.set_num_write_threads(2);
    w.set_write_batch_size(4096);
    w.set_client_default_rpc_timeout_millis(kTimeoutMs);
    w.set_read_timeout_millis(kTimeoutMs);
    w.set_write_timeout_millis(kTimeoutMs);

    auto client = w.CreateClient();
    atomic<bool> importer_do_run(true);
    thread importer([&]() {
      // See below for the explanation.
      MonoTime sync_point = MonoTime::Now();

      while (importer_do_run) {
        // Generate our custom TSK.
        TokenSigningPrivateKeyPB tsk;

        // The master's TokenSigner might be generating TSKs in the background
        // according to its own schedule. The master's TokenSigner increments
        // the sequence number by 1 for every new TSK generated. To avoid TSK
        // sequence number collisions, it's necessary to increment the sequence
        // number for our custom TSKs more aggressively.
        tsk_seq_num += 10;
        CHECK_OK(GenerateTsk(&tsk, tsk_seq_num));

        // Create new authn token, signing it with the custom TSK.
        SignedTokenPB new_signed_token;
        CHECK_OK(GenerateAuthnToken(client, tsk, &new_signed_token));

        ReplaceAuthnToken(client.get(), new_signed_token);
        // From now till the call of ImportTsk() the system is unaware of the
        // custom TSK key and the token signed with it cannot be verified.
        SleepFor(MonoDelta::FromMilliseconds(rand() % 5 + 5));
        CHECK_OK(ImportTsk(tsk));

        // After the ImportTsk() call, the public part of the TSK needs to
        // reach tablet servers so they could verify the custom authn token.
        // The delay is more than the minimum required heartbeat_inteval_ms_
        // to allow for completion of pending operations when retrying with
        // the 'exponential back-off' policy. In addition, some clients might
        // be long in the retry sequence, not being able to catch up with the
        // rate of the token replacement: they need to wake up and make the
        // retry call when current TSK is known to the system. Due to the
        // exponential back-off algorithm of the retry sequence, there might
        // be some clients sleeping for up to ~5 seconds between retries.
        // To avoid timing out in such situations, every timeout interval
        // a 'sync point' happens, so the long-sleeping clients are able to
        // complete their operations. The kSyncSleepIntervalMs is little over
        // the necessary ~5 seconds to avoid test flakiness on slow VMs.
        static const int64_t kSyncSleepIntervalMs = 7500;
        bool is_sync_point =
            (sync_point +
                 MonoDelta::FromMilliseconds(
                     kTimeoutMs - kSyncSleepIntervalMs) <=
             MonoTime::Now());
        const int64_t sleep_time_ms =
            is_sync_point ? kSyncSleepIntervalMs : 5 * heartbeat_interval_ms_;
        SleepFor(MonoDelta::FromMilliseconds(sleep_time_ms));
        if (is_sync_point) {
          sync_point = MonoTime::Now();
        }
      }
    });

    w.Setup();
    w.Start();

    // Let the workload run for some time.
    const int64_t halfRun = kTimeoutMs / 2;
    SleepFor(MonoDelta::FromMilliseconds(rand() % halfRun + halfRun));

    w.StopAndJoin();
    CHECK_OK(w.Cleanup());

    importer_do_run = false;
    importer.join();
  }
}

} // namespace kudu
