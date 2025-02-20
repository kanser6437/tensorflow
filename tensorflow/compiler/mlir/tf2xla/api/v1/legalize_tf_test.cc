/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/tf2xla/api/v1/legalize_tf.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "tensorflow/compiler/mlir/tensorflow/dialect_registration.h"
#include "tensorflow/compiler/mlir/tf2xla/api/v1/device_type.pb.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/compiler/tf2xla/xla_helpers.h"
#include "xla/client/client_library.h"
#include "tensorflow/core/lib/monitoring/cell_reader.h"
#include "tensorflow/core/protobuf/config.pb.h"
#include "tensorflow/core/protobuf/tpu/compile_metadata.pb.h"
#include "tensorflow/core/tpu/kernels/tpu_compile_op_support.h"
#include "tsl/lib/monitoring/test_utils.h"
#include "tsl/platform/statusor.h"

namespace tensorflow {
namespace tf2xla {
namespace v1 {

using ::tensorflow::monitoring::testing::CellReader;
using tpu::FunctionToHloArgs;
using tpu::MlirToHloArgs;
using tpu::ShardingAndIndex;
using tpu::TPUCompileMetadataProto;
using ::tsl::monitoring::testing::Histogram;

static constexpr char kCompilationTimeStreamzName[] =
    "/tensorflow/core/tf2xla/api/v1/phase2_compilation_time";
static constexpr char kCompilationStatusStreamzName[] =
    "/tensorflow/core/tf2xla/api/v1/phase2_compilation_status";
static const char kMlirWithFallbackModeSuccess[] =
    "kMlirWithFallbackModeSuccess";
static const char kMlirWithFallbackModeFailure[] =
    "kMlirWithFallbackModeFailure";
static const char kOldBridgeMlirFilteredFailure[] =
    "kOldBridgeMlirFilteredFailure";
static const char kOldBridgeWithFallbackModeFailure[] =
    "kOldBridgeWithFallbackModeFailure";
static const char kOldBridgeMlirFilteredSuccess[] =
    "kOldBridgeMlirFilteredSuccess";
static const char kOldBridgeWithFallbackModeSuccess[] =
    "kOldBridgeWithFallbackModeSuccess";
static const char kMlirCombinedMlirSuccess[] = "kMlirCombinedMlirSuccess";
static const char kMlirCombinedMlirFailure[] = "kMlirCombinedMlirFailure";
static const char kMlirCombinedOldSuccess[] = "kMlirCombinedOldSuccess";
static const char kMlirCombinedOldFailure[] = "kMlirCombinedOldFailure";

static constexpr char kMlirModuleStr[] = R"(
  module attributes {tf.versions = {bad_consumers = [], min_consumer = 0 : i32, producer = 268 : i32}} {
  func.func @main() -> () {
    func.return
  }
})";

// MLIR which should legalize at all
static constexpr char kBadMlirModuleStr[] = R"(
  module attributes {tf.versions = {bad_consumers = [], min_consumer = 0 : i32, producer = 268 : i32}} {
  func.func @main() -> () {
    %0 = tf.Unknown() -> ()
    func.return %0
  }
})";

// MLIR which should be filtered by the MLIR bridge but fully legalize with the
// combined bridge.
static constexpr char kUnsupportedMlirBridgeModuleStr[] = R"(
  module attributes {tf.versions = {bad_consumers = [], min_consumer = 0 : i32, producer = 268 : i32}} {
  func.func @main() -> () {
    %cst0 = "tf.Const"(){ value = dense<0> : tensor<3x5xi1>} : () -> tensor<3x5xi1>
    %0 = "tf.Where"(%cst0) : (tensor<3x5xi1>) -> tensor<?x2xi64>
    func.return
  }
})";

tsl::StatusOr<XlaCompiler::CompilationResult> CompileMlirModule(
    const char* mlir_module_str,
    ConfigProto::Experimental::MlirBridgeRollout rollout_state) {
  MlirToHloArgs mlir_to_hlo_args;
  mlir_to_hlo_args.rollout_state = rollout_state;
  mlir_to_hlo_args.mlir_module = mlir_module_str;

  se::Platform* platform =
      se::MultiPlatformManager::PlatformWithName("Host").value();
  auto client =
      xla::ClientLibrary::GetOrCreateCompileOnlyClient(platform).value();

  std::vector<TensorShape> arg_shapes;
  TPUCompileMetadataProto metadata_proto;
  bool use_tuple_args = true;
  std::vector<ShardingAndIndex> arg_core_mapping;
  std::vector<std::vector<xla::Shape>> per_core_arg_shapes;
  std::vector<std::unique_ptr<mlir::Pass>> custom_legalization_passes;

  return LegalizeMlirToHlo(mlir_to_hlo_args, metadata_proto, use_tuple_args,
                           /*device_type=*/"XLA_TPU_JIT",
                           custom_legalization_passes,
                           /*shape_determination_fns=*/{}, arg_shapes,
                           &arg_core_mapping, &per_core_arg_shapes, client);
}

TEST(LegalizeTFTest, RecordsStreamzForMlirOpFallback) {
  CellReader<Histogram> compilation_time(kCompilationTimeStreamzName);

  TF_ASSERT_OK_AND_ASSIGN(
      XlaCompiler::CompilationResult result,
      CompileMlirModule(
          kMlirModuleStr,
          ConfigProto::Experimental::MLIR_BRIDGE_ROLLOUT_UNSPECIFIED));

  Histogram histogram =
      compilation_time.Delta("mlir_bridge_op_fallback_enabled");
  EXPECT_EQ(histogram.num(), 1);
}

TEST(LegalizeTFTest, RecordsStreamzForSuccessfulLegalizeWithMlirBridge) {
  CellReader<int64_t> compilation_status(kCompilationStatusStreamzName);

  TF_ASSERT_OK_AND_ASSIGN(
      XlaCompiler::CompilationResult result,
      CompileMlirModule(
          kMlirModuleStr,
          ConfigProto::Experimental::MLIR_BRIDGE_ROLLOUT_UNSPECIFIED));

  // May have been filtered so check for lack of failure instead of success.
  EXPECT_EQ(compilation_status.Delta(kMlirWithFallbackModeFailure), 0);
}

TEST(LegalizeTFTest, RecordsStreamzForFailedLegalizeWithMlirBridge) {
  CellReader<int64_t> compilation_status(kCompilationStatusStreamzName);

  auto result = CompileMlirModule(
      kBadMlirModuleStr,
      ConfigProto::Experimental::MLIR_BRIDGE_ROLLOUT_UNSPECIFIED);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(compilation_status.Delta(kMlirWithFallbackModeSuccess), 0);
  EXPECT_EQ(compilation_status.Delta(kMlirWithFallbackModeFailure), 1);
  EXPECT_EQ(compilation_status.Delta(kMlirCombinedMlirFailure), 1);
}

TEST(LegalizeTFTest, RecordsStreamzForSuccessWithCombinedBridge) {
  CellReader<int64_t> compilation_status(kCompilationStatusStreamzName);

  auto result = CompileMlirModule(
      kUnsupportedMlirBridgeModuleStr,
      ConfigProto::Experimental::MLIR_BRIDGE_ROLLOUT_UNSPECIFIED);

  // MLIR Bridge will filter this unsupported MLIR, Combined will succeed.
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(compilation_status.Delta(kMlirCombinedMlirSuccess), 1);
  EXPECT_EQ(compilation_status.Delta(kMlirCombinedMlirFailure), 0);
  EXPECT_EQ(compilation_status.Delta(kMlirCombinedOldSuccess), 1);
  EXPECT_EQ(compilation_status.Delta(kMlirCombinedOldFailure), 0);
  // Old bridge should never be called at all.
  EXPECT_EQ(compilation_status.Delta(kOldBridgeMlirFilteredFailure), 0);
  EXPECT_EQ(compilation_status.Delta(kOldBridgeWithFallbackModeFailure), 0);
  EXPECT_EQ(compilation_status.Delta(kOldBridgeMlirFilteredSuccess), 0);
  EXPECT_EQ(compilation_status.Delta(kOldBridgeWithFallbackModeSuccess), 0);
}

TEST(LegalizeTFTest, RecordsStreamzForNoMlirFallback) {
  FunctionDef my_func =
      tensorflow::FunctionDefHelper::Create("empty", {}, {}, {}, {}, {});

  tensorflow::FunctionDefLibrary fdef;
  *(fdef.add_function()) = my_func;
  tensorflow::FunctionLibraryDefinition flib_def(
      tensorflow::OpRegistry::Global(), fdef);

  OpInputList guaranteed_constants;
  NameAttrList function;
  FunctionToHloArgs function_to_hlo_args{&function,
                                         &flib_def,
                                         /*graph_def_version=*/0,
                                         {&guaranteed_constants}};

  se::Platform* cpu_platform =
      se::MultiPlatformManager::PlatformWithName("Host").value();
  auto client =
      xla::ClientLibrary::GetOrCreateCompileOnlyClient(cpu_platform).value();

  std::vector<TensorShape> arg_shapes;
  TPUCompileMetadataProto metadata_proto;
  bool use_tuple_args = true;
  std::vector<ShardingAndIndex> arg_core_mapping;
  std::vector<std::vector<xla::Shape>> per_core_arg_shapes;
  std::vector<std::unique_ptr<mlir::Pass>> custom_legalization_passes;

  // This doesn't actually compile correctly.
  tsl::StatusOr<XlaCompiler::CompilationResult> compile_result =
      LegalizeMlirToHlo(function_to_hlo_args, metadata_proto, use_tuple_args,
                        /*device_type=*/"XLA_CPU_JIT",
                        custom_legalization_passes,
                        /*shape_determination_fns=*/{}, arg_shapes,
                        &arg_core_mapping, &per_core_arg_shapes, client);

  EXPECT_FALSE(compile_result.ok());
}

}  // namespace v1
}  // namespace tf2xla
}  // namespace tensorflow
