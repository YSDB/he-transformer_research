//*****************************************************************************
// Copyright 2018-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include "seal/he_seal_executable.hpp"

#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "he_op_annotations.hpp"
#include "he_tensor.hpp"
#include "ngraph/descriptor/layout/dense_tensor_layout.hpp"
#include "ngraph/ops.hpp"
#include "ngraph/pass/assign_layout.hpp"
#include "ngraph/pass/constant_folding.hpp"
#include "ngraph/pass/core_fusion.hpp"
#include "ngraph/pass/like_replacement.hpp"
#include "ngraph/pass/liveness.hpp"
#include "ngraph/pass/manager.hpp"
#include "ngraph/pass/memory_layout.hpp"
#include "ngraph/pass/visualize_tree.hpp"
#include "ngraph/runtime/backend.hpp"
#include "ngraph/serializer.hpp"
#include "ngraph/util.hpp"
#include "nlohmann/json.hpp"
#include "op/bounded_relu.hpp"
#include "pass/he_fusion.hpp"
#include "pass/he_liveness.hpp"
#include "pass/propagate_he_annotations.hpp"
#include "pass/supported_ops.hpp"
#include "protos/message.pb.h"
#include "seal/he_seal_backend.hpp"
#include "seal/kernel/add_seal.hpp"
#include "seal/kernel/avg_pool_seal.hpp"
#include "seal/kernel/batch_norm_inference_seal.hpp"
#include "seal/kernel/bounded_relu_seal.hpp"
#include "seal/kernel/broadcast_seal.hpp"
#include "seal/kernel/concat_seal.hpp"
#include "seal/kernel/constant_seal.hpp"
#include "seal/kernel/convolution_seal.hpp"
#include "seal/kernel/divide_seal.hpp"
#include "seal/kernel/dot_seal.hpp"
#include "seal/kernel/exp_seal.hpp"
#include "seal/kernel/max_pool_seal.hpp"
#include "seal/kernel/max_seal.hpp"
#include "seal/kernel/minimum_seal.hpp"
#include "seal/kernel/mod_reduce_seal.hpp"
#include "seal/kernel/multiply_seal.hpp"
#include "seal/kernel/negate_seal.hpp"
#include "seal/kernel/pad_seal.hpp"
#include "seal/kernel/power_seal.hpp"
#include "seal/kernel/relu_seal.hpp"
#include "seal/kernel/rescale_seal.hpp"
#include "seal/kernel/reshape_seal.hpp"
#include "seal/kernel/result_seal.hpp"
#include "seal/kernel/reverse_seal.hpp"
#include "seal/kernel/slice_seal.hpp"
#include "seal/kernel/softmax_seal.hpp"
#include "seal/kernel/subtract_seal.hpp"
#include "seal/kernel/sum_seal.hpp"
#include "seal/seal_ciphertext_wrapper.hpp"
#include "seal/seal_util.hpp"

using json = nlohmann::json;
using ngraph::descriptor::layout::DenseTensorLayout;

namespace ngraph::runtime::he {

HESealExecutable::HESealExecutable(const std::shared_ptr<Function>& function,
                                   bool enable_performance_collection,
                                   HESealBackend& he_seal_backend)
    : m_he_seal_backend(he_seal_backend), m_batch_size{1} {
  // TODO(fboemer): Use
  (void)enable_performance_collection;  // Avoid unused parameter warning

  m_context = he_seal_backend.get_context();
  m_port = he_seal_backend.port();
  m_function = function;

  if (!m_context->using_keyswitching()) {
    m_client_eval_key_set = true;
  }

  NGRAPH_HE_LOG(3) << "Creating Executable";
  for (const auto& param : m_function->get_parameters()) {
    NGRAPH_HE_LOG(3) << "Parameter " << param->get_name();
    if (HEOpAnnotations::has_he_annotation(*param)) {
      std::string from_client_str =
          HEOpAnnotations::from_client(*param) ? "" : "not ";
      NGRAPH_HE_LOG(3) << "\tshape " << param->get_shape() << " is "
                       << from_client_str << "from client";
    }

    for (const auto& tag : param->get_provenance_tags()) {
      NGRAPH_HE_LOG(3) << "\tTag " << tag;
    }
  }

  if (std::getenv("NGRAPH_HE_VERBOSE_OPS") != nullptr) {
    std::string verbose_ops_str(std::getenv("NGRAPH_HE_VERBOSE_OPS"));
    verbose_ops_str = to_lower(verbose_ops_str);
    if (verbose_ops_str == "all") {
      m_verbose_all_ops = true;
    }
    std::vector<std::string> verbose_ops_vec =
        split(verbose_ops_str, ',', true);
    m_verbose_ops =
        std::set<std::string>{verbose_ops_vec.begin(), verbose_ops_vec.end()};

    if (m_verbose_ops.find("all") != m_verbose_ops.end()) {
      m_verbose_all_ops = true;
    }
  }

  NGRAPH_HE_LOG(3) << "Running optimization passes";
  ngraph::pass::Manager pass_manager;
  pass_manager.set_pass_visualization(false);
  pass_manager.set_pass_serialization(false);

  pass_manager.register_pass<ngraph::pass::LikeReplacement>();
  pass_manager.register_pass<ngraph::pass::AssignLayout<DenseTensorLayout>>();
  pass_manager.register_pass<ngraph::pass::CoreFusion>();
  pass_manager.register_pass<ngraph::pass::ConstantFolding>();

  NGRAPH_HE_LOG(4) << "Running passes";
  pass_manager.run_passes(m_function);

  ngraph::pass::Manager pass_manager_he;
  pass_manager_he.set_pass_visualization(false);
  pass_manager_he.set_pass_serialization(false);
  pass_manager_he.register_pass<pass::HEFusion>();
  pass_manager_he.register_pass<pass::HELiveness>();
  pass_manager_he.register_pass<pass::SupportedOps>(
      [this](const Node& op) { return m_he_seal_backend.is_supported(op); });

  NGRAPH_HE_LOG(4) << "Running HE passes";
  pass_manager_he.run_passes(m_function);

  update_he_op_annotations();
}

HESealExecutable::~HESealExecutable() noexcept {
  NGRAPH_HE_LOG(3) << "~HESealExecutable()";
  if (m_server_setup) {
    if (m_message_handling_thread.joinable()) {
      NGRAPH_HE_LOG(5) << "Waiting for m_message_handling_thread to join";
      try {
        m_message_handling_thread.join();
      } catch (std::exception& e) {
        NGRAPH_ERR << "Exception closing executable thread " << e.what();
      }
      NGRAPH_HE_LOG(5) << "m_message_handling_thread joined";
    }

    // m_acceptor and m_io_context both free the socket? Avoid double-free
    try {
      m_acceptor->close();
    } catch (std::exception& e) {
      NGRAPH_ERR << "Exception closing m_acceptor " << e.what();
    }
    m_acceptor = nullptr;
    m_session = nullptr;
  }
}

void HESealExecutable::update_he_op_annotations() {
  NGRAPH_HE_LOG(3) << "Upadting HE op annotations";
  ngraph::pass::Manager pass_manager_he;
  pass_manager_he.register_pass<pass::PropagateHEAnnotations>();
  pass_manager_he.run_passes(m_function);
  m_is_compiled = true;

  m_nodes.clear();
  for (auto node : m_function->get_ordered_ops()) {
    m_nodes.push_back(node);
  }
  set_parameters_and_results(*m_function);
}

size_t HESealExecutable::batch_size() const { return m_batch_size; }

void HESealExecutable::set_batch_size(size_t batch_size) {
  size_t max_batch_size = m_he_seal_backend.get_ckks_encoder()->slot_count();
  if (complex_packing()) {
    max_batch_size *= 2;
  }
  NGRAPH_CHECK(batch_size <= max_batch_size, "Batch size ", batch_size,
               " too large (maximum ", max_batch_size, ")");
  m_batch_size = batch_size;

  NGRAPH_HE_LOG(5) << "Server set batch size to " << m_batch_size;
}

void HESealExecutable::set_verbose_all_ops(bool value) {
  m_verbose_all_ops = value;
}

OP_TYPEID HESealExecutable::get_typeid(const NodeTypeInfo& type_info) {
  {
    // This expands the op list in op_tbl.hpp into a list of enumerations that
    // look like this: {Abs::type_info, OP_TYPEID::Abs}, {Acos::type_info,
    // OP_TYPEID::Acos},
    // ...
    static const std::map<NodeTypeInfo, OP_TYPEID> type_info_map{
#define NGRAPH_OP(NAME, NAMESPACE) \
  {NAMESPACE::NAME::type_info, OP_TYPEID::ID_SUFFIX(NAME)},
#include "opset_he_seal_tbl.hpp"
#undef NGRAPH_OP
    };
    OP_TYPEID rc = OP_TYPEID::UnknownOp;

    auto it = type_info_map.find(type_info);
    if (it != type_info_map.end()) {
      rc = it->second;
    }
    return rc;
  }
}

void HESealExecutable::check_client_supports_function() {
  // Check if single parameter is from client
  size_t from_client_count = 0;
  for (const auto& param : get_parameters()) {
    if (HEOpAnnotations::from_client(*param)) {
      from_client_count++;
      NGRAPH_HE_LOG(5) << "Parameter " << param->get_name() << " from client";
    }
  }
  NGRAPH_CHECK(get_results().size() == 1,
               "HESealExecutable only supports output size 1 (got ",
               get_results().size(), ")");
  NGRAPH_CHECK(from_client_count > 0, "Expected > 0 parameters from client");
}

bool HESealExecutable::server_setup() {
  if (!m_server_setup) {
    NGRAPH_HE_LOG(1) << "Enable client";

    check_client_supports_function();

    NGRAPH_HE_LOG(1) << "Starting server";
    start_server();

#ifdef NGRAPH_HE_ABY_ENABLE
    if (enable_garbled_circuits()) {
      m_aby_executor = std::make_unique<aby::ABYServerExecutor>(
          *this, std::string("yao"), std::string("0.0.0.0"), 34001, 128, 64, 2,
          m_he_seal_backend.num_garbled_circuit_threads());
    }
#endif

    std::stringstream param_stream;
    m_he_seal_backend.get_encryption_parameters().save(param_stream);

    pb::EncryptionParameters pb_params;
    *pb_params.mutable_encryption_parameters() = param_stream.str();

    pb::TCPMessage pb_message;
    *pb_message.mutable_encryption_parameters() = pb_params;
    pb_message.set_type(pb::TCPMessage_Type_RESPONSE);

    TCPMessage parms_message(std::move(pb_message));
    NGRAPH_HE_LOG(3) << "Server waiting until session started";
    std::unique_lock<std::mutex> mlock(m_session_mutex);
    m_session_cond.wait(mlock, [this]() { return this->session_started(); });

    NGRAPH_HE_LOG(3) << "Server writing parameters message";
    m_session->write_message(std::move(parms_message));
    m_server_setup = true;

    // Set client inputs to dummy values
    if (m_is_compiled) {
      m_client_inputs.clear();
      m_client_inputs.resize(get_parameters().size());
    }
  } else {
    NGRAPH_HE_LOG(1) << "Client already setup";
  }
  return true;
}

void HESealExecutable::accept_connection() {
  NGRAPH_HE_LOG(1) << "Server accepting connections";
  auto server_callback =
      std::bind(&HESealExecutable::handle_message, this, std::placeholders::_1);

  m_acceptor->async_accept(
      [this, server_callback](boost::system::error_code ec,
                              boost::asio::ip::tcp::socket socket) {
        if (!ec) {
          NGRAPH_HE_LOG(1) << "Connection accepted";
          m_session =
              std::make_shared<TCPSession>(std::move(socket), server_callback);
          m_session->start();
          NGRAPH_HE_LOG(1) << "Session started";

          std::lock_guard<std::mutex> guard(m_session_mutex);
          m_session_started = true;
          m_session_cond.notify_one();
        } else {
          NGRAPH_ERR << "error accepting connection " << ec.message();
          accept_connection();
        }
      });
}

void HESealExecutable::start_server() {
  boost::asio::ip::tcp::resolver resolver(m_io_context);
  boost::asio::ip::tcp::endpoint server_endpoints(boost::asio::ip::tcp::v4(),
                                                  m_port);
  m_acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(
      m_io_context, server_endpoints);
  boost::asio::socket_base::reuse_address option(true);
  m_acceptor->set_option(option);

  accept_connection();
  m_message_handling_thread = std::thread([this]() {
    try {
      m_io_context.run();
    } catch (std::exception& e) {
      NGRAPH_CHECK(false, "Server error handling thread: ", e.what());
    }
  });
}

void HESealExecutable::load_public_key(const pb::TCPMessage& pb_message) {
  NGRAPH_HE_LOG(5) << "Server loading evaluation key";
  NGRAPH_CHECK(pb_message.has_public_key(),
               "pb_message doesn't have public key");

  seal::PublicKey key;
  const std::string& pk_str = pb_message.public_key().public_key();
  std::stringstream key_stream(pk_str);
  key.load(*m_context, key_stream);
  m_he_seal_backend.set_public_key(key);
  m_client_public_key_set = true;
}

void HESealExecutable::load_eval_key(const pb::TCPMessage& pb_message) {
  NGRAPH_HE_LOG(5) << "Server loading evaluation key";
  NGRAPH_CHECK(pb_message.has_eval_key(), "pb_message doesn't have eval key");

  seal::RelinKeys keys;
  const std::string& evk_str = pb_message.eval_key().eval_key();
  std::stringstream key_stream(evk_str);
  keys.load(*m_context, key_stream);
  m_he_seal_backend.set_relin_keys(keys);
  m_client_eval_key_set = true;
}

void HESealExecutable::send_inference_shape() {
  m_sent_inference_shape = true;

  const ParameterVector& input_parameters = get_parameters();

  pb::TCPMessage pb_message;
  pb_message.set_type(pb::TCPMessage_Type_REQUEST);

  for (const auto& input_param : input_parameters) {
    if (HEOpAnnotations::from_client(*input_param)) {
      pb::HETensor* pb_tensor = pb_message.add_he_tensors();

      std::vector<uint64_t> shape{input_param->get_shape()};
      *pb_tensor->mutable_shape() = {shape.begin(), shape.end()};

      std::string name = input_param->get_provenance_tags().empty()
                             ? input_param->get_name()
                             : *input_param->get_provenance_tags().begin();

      NGRAPH_HE_LOG(1) << "Server setting inference tensor name " << name
                       << " (corresponding to Parameter "
                       << input_param->get_name() << "), with "
                       << input_param->get_shape();

      pb_tensor->set_name(name);

      if (HEOpAnnotations::plaintext_packed(*input_param)) {
        NGRAPH_HE_LOG(1) << "Setting parameter " << input_param->get_name()
                         << " to packed";
        pb_tensor->set_packed(true);
      }
    }
  }

  NGRAPH_HE_LOG(1) << "Server sending inference of "
                   << pb_message.he_tensors_size() << " parameters";

  json js = {{"function", "Parameter"}};
  pb::Function f;
  f.set_function(js.dump());
  NGRAPH_HE_LOG(3) << "js " << js.dump();
  *pb_message.mutable_function() = f;
  m_session->write_message(TCPMessage(std::move(pb_message)));
}

void HESealExecutable::handle_relu_result(const pb::TCPMessage& pb_message) {
  NGRAPH_HE_LOG(3) << "Server handling relu result";
  std::lock_guard<std::mutex> guard(m_relu_mutex);

  NGRAPH_CHECK(pb_message.he_tensors_size() == 1,
               "Can only handle one tensor at a time, got ",
               pb_message.he_tensors_size());

  const auto& pb_tensor = pb_message.he_tensors(0);
  auto he_tensor = HETensor::load_from_pb_tensor(
      pb_tensor, *m_he_seal_backend.get_ckks_encoder(),
      m_he_seal_backend.get_context(), *m_he_seal_backend.get_encryptor(),
      *m_he_seal_backend.get_decryptor(),
      m_he_seal_backend.get_encryption_parameters());

  size_t result_count = pb_tensor.data_size();
  for (size_t result_idx = 0; result_idx < result_count; ++result_idx) {
    m_relu_data[m_unknown_relu_idx[result_idx + m_relu_done_count]] =
        he_tensor->data(result_idx);
  }

#ifdef NGRAPH_HE_ABY_ENABLE
  if (enable_garbled_circuits()) {
    m_aby_executor->post_process_aby_circuit(pb_message.function().function(),
                                             he_tensor);
  }
#endif

  m_relu_done_count += result_count;
  m_relu_cond.notify_all();
}

void HESealExecutable::handle_bounded_relu_result(
    const pb::TCPMessage& pb_message) {
  handle_relu_result(pb_message);
}

void HESealExecutable::handle_max_pool_result(
    const pb::TCPMessage& pb_message) {
  std::lock_guard<std::mutex> guard(m_max_pool_mutex);

  NGRAPH_CHECK(pb_message.he_tensors_size() == 1,
               "Can only handle one tensor at a time, got ",
               pb_message.he_tensors_size());

  const auto& pb_tensor = pb_message.he_tensors(0);
  size_t result_count = pb_tensor.data_size();

  NGRAPH_CHECK(result_count == 1, "Maxpool only supports result_count 1, got ",
               result_count);

  auto he_tensor = HETensor::load_from_pb_tensor(
      pb_tensor, *m_he_seal_backend.get_ckks_encoder(),
      m_he_seal_backend.get_context(), *m_he_seal_backend.get_encryptor(),
      *m_he_seal_backend.get_decryptor(),
      m_he_seal_backend.get_encryption_parameters());

  m_max_pool_data.emplace_back(he_tensor->data(0));
  m_max_pool_done = true;
  m_max_pool_cond.notify_all();
}

void HESealExecutable::handle_message(const TCPMessage& message) {
  NGRAPH_HE_LOG(3) << "Server handling message";
  std::shared_ptr<pb::TCPMessage> pb_message = message.pb_message();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
  switch (pb_message->type()) {
    case pb::TCPMessage_Type_RESPONSE: {
      if (pb_message->has_public_key()) {
        load_public_key(*pb_message);
      }
      if (pb_message->has_eval_key()) {
        load_eval_key(*pb_message);
      }
      if (!m_sent_inference_shape && m_client_public_key_set &&
          m_client_eval_key_set) {
        send_inference_shape();
      }

      if (pb_message->has_function()) {
        const std::string& function = pb_message->function().function();
        json js = json::parse(function);

        auto name = js.at("function");

        static std::unordered_set<std::string> known_function_names{
            "Relu", "BoundedRelu", "MaxPool"};
        NGRAPH_CHECK(
            known_function_names.find(name) != known_function_names.end(),
            "Unknown function name ", name);

        if (name == "Relu") {
          handle_relu_result(*pb_message);
        } else if (name == "BoundedRelu") {
          handle_bounded_relu_result(*pb_message);
        } else if (name == "MaxPool") {
          handle_max_pool_result(*pb_message);
        }
      }
      break;
    }
    case pb::TCPMessage_Type_REQUEST: {
      if (pb_message->he_tensors_size() > 0) {
        handle_client_ciphers(*pb_message);
      }
      break;
    }
    case pb::TCPMessage_Type_UNKNOWN:
    default:
      NGRAPH_CHECK(false, "Unknonwn TCPMessage type");
  }
#pragma clang diagnostic pop
}

void HESealExecutable::handle_client_ciphers(const pb::TCPMessage& pb_message) {
  NGRAPH_HE_LOG(3) << "Handling client tensors";

  NGRAPH_CHECK(pb_message.he_tensors_size() > 0,
               "Client received empty tensor message");
  NGRAPH_CHECK(pb_message.he_tensors_size() == 1,
               "Client only supports 1 client tensor");
  // TODO(fboemer): check for uniqueness of batch size if > 1 input tensor

  const ParameterVector& input_parameters = get_parameters();

  /// Looks for a parameter which matches a given tensor name
  /// tensor_name Tensor name to match against
  /// \returns parameter_idx if a matching parameter shape has been found,
  /// std::nullopt otherwise
  auto find_matching_parameter_index =
      [&](const std::string& tensor_name) -> std::optional<size_t> {
    NGRAPH_HE_LOG(5) << "Calling find_matching_parameter_index(" << tensor_name
                     << ")";
    for (size_t param_idx = 0; param_idx < input_parameters.size();
         ++param_idx) {
      const auto& parameter = input_parameters[param_idx];

      for (const auto& tag : parameter->get_provenance_tags()) {
        NGRAPH_HE_LOG(5) << "Tag " << tag;
      }

      if (param_originates_from_name(*parameter, tensor_name)) {
        NGRAPH_HE_LOG(5) << "Param " << tensor_name << " matches at index "
                         << param_idx;
        return std::optional<size_t>{param_idx};
      }
    }
    NGRAPH_HE_LOG(5) << "Could not find tensor " << tensor_name;
    return std::nullopt;
  };

  auto& pb_tensor = pb_message.he_tensors(0);
  ngraph::Shape shape{pb_tensor.shape().begin(), pb_tensor.shape().end()};

  NGRAPH_HE_LOG(5) << "pb_tensor.packed() " << pb_tensor.packed();
  set_batch_size(HETensor::batch_size(shape, pb_tensor.packed()));
  NGRAPH_HE_LOG(5) << "Offset " << pb_tensor.offset();

  std::optional<size_t> param_idx =
      find_matching_parameter_index(pb_tensor.name());
  NGRAPH_CHECK(param_idx, "Could not find matching parameter name ",
               pb_tensor.name());

  if (m_client_inputs[param_idx.value()] == nullptr) {
    auto he_tensor = HETensor::load_from_pb_tensor(
        pb_tensor, *m_he_seal_backend.get_ckks_encoder(),
        m_he_seal_backend.get_context(), *m_he_seal_backend.get_encryptor(),
        *m_he_seal_backend.get_decryptor(),
        m_he_seal_backend.get_encryption_parameters());
    m_client_inputs[param_idx.value()] = he_tensor;
  } else {
    HETensor::load_from_pb_tensor(m_client_inputs[param_idx.value()], pb_tensor,
                                  m_he_seal_backend.get_context());
  }

  auto done_loading = [&]() {
    for (size_t parm_idx = 0; parm_idx < input_parameters.size(); ++parm_idx) {
      const auto& param = input_parameters[parm_idx];
      if (HEOpAnnotations::from_client(*param)) {
        NGRAPH_HE_LOG(5) << "From client param shape " << param->get_shape();
        NGRAPH_HE_LOG(5) << "m_batch_size " << m_batch_size;

        if (m_client_inputs[parm_idx] == nullptr ||
            !m_client_inputs[parm_idx]->done_loading()) {
          return false;
        }
      }
    }
    return true;
  };

  if (done_loading()) {
    NGRAPH_HE_LOG(3) << "Done loading client ciphertexts";

    std::lock_guard<std::mutex> guard(m_client_inputs_mutex);
    m_client_inputs_received = true;
    NGRAPH_HE_LOG(5) << "Notifying done loading client ciphertexts";
    m_client_inputs_cond.notify_all();
  } else {
    NGRAPH_HE_LOG(3) << "Not yet done loading client ciphertexts";
  }
}

std::vector<runtime::PerformanceCounter>
HESealExecutable::get_performance_data() const {
  std::vector<runtime::PerformanceCounter> rc;
  for (const auto& [node, stop_watch] : m_timer_map) {
    rc.emplace_back(node, stop_watch.get_total_microseconds(),
                    stop_watch.get_call_count());
  }
  return rc;
}

bool HESealExecutable::call(
    const std::vector<std::shared_ptr<runtime::Tensor>>& outputs,
    const std::vector<std::shared_ptr<runtime::Tensor>>& server_inputs) {
  NGRAPH_HE_LOG(3) << "HESealExecutable::call";
  validate(outputs, server_inputs);
  NGRAPH_HE_LOG(3) << "HESealExecutable::call validated inputs";

  if (enable_client()) {
    if (!server_setup()) {
      return false;
    }
  }

  if (complex_packing()) {
    NGRAPH_HE_LOG(1) << "Complex packing";
  }

  if (enable_client()) {
    NGRAPH_HE_LOG(1) << "Waiting for m_client_inputs";

    std::unique_lock<std::mutex> mlock(m_client_inputs_mutex);
    m_client_inputs_cond.wait(
        mlock, std::bind(&HESealExecutable::client_inputs_received, this));
    NGRAPH_HE_LOG(1) << "Client inputs_received";
  }

  // convert inputs to HETensor
  NGRAPH_HE_LOG(3) << "Converting inputs to HETensor";
  const auto& parameters = get_parameters();
  std::vector<std::shared_ptr<HETensor>> he_inputs;
  for (size_t input_idx = 0; input_idx < server_inputs.size(); ++input_idx) {
    auto param_shape = server_inputs[input_idx]->get_shape();
    auto& param = parameters[input_idx];
    std::shared_ptr<HETensor> he_input;

    if (enable_client() && HEOpAnnotations::from_client(*param)) {
      NGRAPH_HE_LOG(1) << "Processing parameter " << param->get_name()
                       << "(shape {" << param_shape << "}) from client";
      NGRAPH_CHECK(m_client_inputs.size() > input_idx,
                   "Not enough client inputs");
      he_input = std::static_pointer_cast<HETensor>(m_client_inputs[input_idx]);

      auto current_annotation = HEOpAnnotations::he_op_annotation(*param);
      current_annotation->set_encrypted(he_input->any_encrypted_data());
    } else {
      NGRAPH_HE_LOG(1) << "Processing parameter " << param->get_name()
                       << "(shape {" << param_shape << "}) from server";

      he_input = std::static_pointer_cast<HETensor>(server_inputs[input_idx]);
      auto current_annotation = HEOpAnnotations::he_op_annotation(*param);

      NGRAPH_HE_LOG(5) << "Parameter " << param->get_name()
                       << " has annotation " << *current_annotation;
      if (!he_input->any_encrypted_data()) {
        if (current_annotation->packed()) {
          he_input->pack();
        } else {
          he_input->unpack();
        }
      }

      if (current_annotation->encrypted()) {
        NGRAPH_HE_LOG(3) << "Encrypting parameter " << param->get_name()
                         << " from server";
#pragma omp parallel for
        for (size_t he_type_idx = 0;
             he_type_idx < he_input->get_batched_element_count();
             ++he_type_idx) {
          if (he_input->data(he_type_idx).is_plaintext()) {
            auto cipher = HESealBackend::create_empty_ciphertext();
            m_he_seal_backend.encrypt(
                cipher, he_input->data(he_type_idx).get_plaintext(),
                he_input->get_element_type(),
                he_input->data(he_type_idx).complex_packing());
            he_input->data(he_type_idx).set_ciphertext(cipher);
          }
        }
        NGRAPH_HE_LOG(3) << "Done encrypting parameter " << param->get_name()
                         << " from server";
      }
    }
    NGRAPH_CHECK(he_input != nullptr, "HE input is nullptr");
    NGRAPH_CHECK(
        he_input->is_packed() ==
            HEOpAnnotations::he_op_annotation(*param)->packed(),
        "Mismatch between tensor input and annotation (", he_input->is_packed(),
        " != ", HEOpAnnotations::he_op_annotation(*param)->packed(), ")");
    if (he_input->is_packed()) {
      set_batch_size(he_input->get_batch_size());
    }
    he_inputs.emplace_back(he_input);
  }

  NGRAPH_HE_LOG(3) << "Updating HE op annotations";
  update_he_op_annotations();

  NGRAPH_HE_LOG(3) << "Converting outputs to HETensor";
  std::vector<std::shared_ptr<HETensor>> he_outputs;
  he_outputs.reserve(outputs.size());
  for (auto& tensor : outputs) {
    he_outputs.push_back(std::static_pointer_cast<HETensor>(tensor));
  }

  NGRAPH_HE_LOG(3) << "Mapping function parameters to HETensor";
  NGRAPH_CHECK(he_inputs.size() >= parameters.size(),
               "Not enough inputs in input map");
  std::unordered_map<descriptor::Tensor*, std::shared_ptr<HETensor>> tensor_map;
  size_t input_count = 0;
  for (const auto& param : parameters) {
    for (size_t param_out_idx = 0; param_out_idx < param->get_output_size();
         ++param_out_idx) {
      descriptor::Tensor* tensor =
          param->get_output_tensor_ptr(param_out_idx).get();
      tensor_map.insert({tensor, he_inputs[input_count++]});
    }
  }

  NGRAPH_HE_LOG(3) << "Mapping function outputs to HETensor";
  for (size_t output_count = 0; output_count < get_results().size();
       ++output_count) {
    std::shared_ptr<op::Result> output = get_results()[output_count];
    ngraph::descriptor::Tensor* tv = output->get_output_tensor_ptr(0).get();

    auto& he_output = he_outputs[output_count];

    if (HEOpAnnotations::has_he_annotation(*output)) {
      auto he_op_annotation = HEOpAnnotations::he_op_annotation(*output);
      if (!he_output->any_encrypted_data()) {
        if (he_op_annotation->packed()) {
          he_output->pack();
        } else {
          he_output->unpack();
        }
      }
    }
    tensor_map.insert({tv, he_output});
  }

  // for each ordered op in the graph
  for (auto op : m_nodes) {
    NGRAPH_CHECK(op->is_op(), "Not is not an op");
    bool verbose = verbose_op(op.get());

    if (verbose) {
      NGRAPH_HE_LOG(3) << "\033[1;32m"
                       << "[ " << op->get_name() << " ]"
                       << "\033[0m";
      if (op->is_constant()) {
        NGRAPH_HE_LOG(3) << "Constant shape " << op->get_shape();
      }
    }

    if (op->is_parameter()) {
      if (verbose) {
        const auto param_op = std::static_pointer_cast<const op::Parameter>(op);
        if (HEOpAnnotations::has_he_annotation(*param_op)) {
          std::string from_client_str =
              HEOpAnnotations::from_client(*param_op) ? "" : " not";
          NGRAPH_HE_LOG(3) << "Parameter shape " << param_op->get_shape()
                           << from_client_str << " from client";
        }
      }
      continue;
    }
    m_timer_map[op].start();

    // get op inputs from map
    std::vector<std::shared_ptr<HETensor>> op_inputs;
    for (auto input : op->inputs()) {
      descriptor::Tensor* tensor = &input.get_tensor();
      op_inputs.push_back(tensor_map.at(tensor));
    }

    if (enable_client() && op->is_output()) {
      // Client outputs don't have decryption performed, so skip result op
      NGRAPH_HE_LOG(3) << "Setting client outputs";
      m_client_outputs = op_inputs;
    }

    // get op outputs from map or create
    std::vector<std::shared_ptr<HETensor>> op_outputs;
    for (size_t i = 0; i < op->get_output_size(); ++i) {
      auto tensor = &op->output(i).get_tensor();
      auto it = tensor_map.find(tensor);
      if (it == tensor_map.end()) {
        // The output tensor is not in the tensor map so create a new tensor
        Shape shape = op->get_output_shape(i);
        const element::Type& element_type = op->get_output_element_type(i);
        std::string name = op->output(i).get_tensor().get_name();

        NGRAPH_HE_LOG(3) << "Get output packing / encrypted";

        std::shared_ptr<HEOpAnnotations> he_op_annotation =
            HEOpAnnotations::he_op_annotation(*static_cast<op::Op*>(op.get()));
        bool encrypted_out = he_op_annotation->encrypted();
        bool packed_out = he_op_annotation->packed();

        NGRAPH_HE_LOG(3) << "encrypted_out " << encrypted_out;
        NGRAPH_HE_LOG(3) << "packed_out " << packed_out;
        if (packed_out) {
          shape = HETensor::unpack_shape(shape, batch_size());
        }
        NGRAPH_HE_LOG(5) << "Creating output tensor with shape " << shape;

        if (encrypted_out) {
          auto out_tensor = std::static_pointer_cast<HETensor>(
              m_he_seal_backend.create_cipher_tensor(element_type, shape,
                                                     packed_out, name));
          tensor_map.insert({tensor, out_tensor});
        } else {
          auto out_tensor = std::static_pointer_cast<HETensor>(
              m_he_seal_backend.create_plain_tensor(element_type, shape,
                                                    packed_out, name));
          tensor_map.insert({tensor, out_tensor});
        }
      }
      op_outputs.push_back(tensor_map.at(tensor));
    }

    // get op type
    element::Type base_type;
    if (op->get_inputs().empty()) {
      base_type = op->get_element_type();
    } else {
      base_type = op->get_inputs().at(0).get_tensor().get_element_type();
    }

    generate_calls(base_type, *op.get(), op_outputs, op_inputs);
    m_timer_map[op].stop();

    // delete any obsolete tensors
    for (const descriptor::Tensor* t : op->liveness_free_list) {
      bool erased = false;
      for (auto it = tensor_map.begin(); it != tensor_map.end(); ++it) {
        const std::string& it_name = it->second->get_name();
        if (it_name == t->get_name()) {
          tensor_map.erase(it);
          erased = true;
          break;
        }
      }
      if (!erased) {
        NGRAPH_HE_LOG(5) << "Failed to erase " << t->get_name()
                         << " from tensor map";
      }
    }
    if (verbose) {
      NGRAPH_HE_LOG(3) << "\033[1;31m" << op->get_name() << " took "
                       << m_timer_map[op].get_milliseconds() << "ms"
                       << "\033[0m";
    }
  }
  size_t total_time = 0;
  for (const auto& elem : m_timer_map) {
    total_time += elem.second.get_milliseconds();
  }
  if (verbose_op("total")) {
    NGRAPH_HE_LOG(3) << "\033[1;32m"
                     << "Total time " << total_time << " (ms) \033[0m";
  }

  // Send outputs to client.
  if (enable_client()) {
    send_client_results();
  }
  return true;
}

void HESealExecutable::send_client_results() {
  NGRAPH_HE_LOG(3) << "Sending results to client";
  NGRAPH_CHECK(m_client_outputs.size() == 1,
               "HESealExecutable only supports output size 1 (got ",
               get_results().size(), "");

  auto pb_tensors = m_client_outputs[0]->write_to_pb_tensors();

  for (const auto& pb_tensor : pb_tensors) {
    pb::TCPMessage result_msg;
    result_msg.set_type(pb::TCPMessage_Type_RESPONSE);
    *result_msg.add_he_tensors() = pb_tensor;

    auto result_shape = result_msg.he_tensors(0).shape();
    NGRAPH_HE_LOG(3) << "Server sending result with shape "
                     << Shape{result_shape.begin(), result_shape.end()};
    m_session->write_message(TCPMessage(std::move(result_msg)));
  }

  // Wait until message is written
  std::unique_lock<std::mutex> mlock(m_result_mutex);
  std::condition_variable& writing_cond = m_session->is_writing_cond();
  writing_cond.wait(mlock, [this] { return !m_session->is_writing(); });
}

void HESealExecutable::generate_calls(
    const element::Type& type, const Node& node,
    const std::vector<std::shared_ptr<HETensor>>& out,
    const std::vector<std::shared_ptr<HETensor>>& args) {
  bool verbose = verbose_op(&node);

// We want to check that every OP_TYPEID enumeration is included in the
// list. These clang flags enable compile-time checking so that if an
//      enumeration
// is not in the list an error is generated.
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wswitch-enum"
  switch (get_typeid(node.get_type_info())) {
    case OP_TYPEID::Add: {
      // Avoid lazy mod for single add op
      if (m_he_seal_backend.lazy_mod()) {
        m_he_seal_backend.lazy_mod() = false;
        add_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                 out[0]->get_batched_element_count(), type, m_he_seal_backend);
        m_he_seal_backend.lazy_mod() = true;
      } else {
        add_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                 out[0]->get_batched_element_count(), type, m_he_seal_backend);
      }
      break;
    }
    case OP_TYPEID::AvgPool: {
      const auto avg_pool = static_cast<const op::AvgPool*>(&node);
      Shape op_in_shape = args[0]->get_packed_shape();
      Shape op_out_shape = out[0]->get_packed_shape();

      if (verbose) {
        NGRAPH_HE_LOG(3) << "AvgPool " << op_in_shape << " => " << op_out_shape;
      }

      avg_pool_seal(
          args[0]->data(), out[0]->data(), op_in_shape, op_out_shape,
          avg_pool->get_window_shape(), avg_pool->get_window_movement_strides(),
          avg_pool->get_padding_below(), avg_pool->get_padding_above(),
          avg_pool->get_include_padding_in_avg_computation(),
          out[0]->get_batch_size(), m_he_seal_backend);

      if (m_he_seal_backend.lazy_mod()) {
        mod_reduce_seal(out[0]->data(), m_he_seal_backend, verbose);
      }
      rescale_seal(out[0]->data(), m_he_seal_backend, verbose);
      break;
    }
    case OP_TYPEID::BatchNormInference: {
      const auto bn = static_cast<const op::BatchNormInference*>(&node);
      double eps = bn->get_eps_value();
      NGRAPH_CHECK(args.size() == 5, "BatchNormInference has ", args.size(),
                   "arguments (expected 5).");

      auto gamma = args[0];
      auto beta = args[1];
      auto input = args[2];
      auto mean = args[3];
      auto variance = args[4];

      batch_norm_inference_seal(eps, gamma->data(), beta->data(), input->data(),
                                mean->data(), variance->data(), out[0]->data(),
                                args[2]->get_packed_shape(), batch_size(),
                                m_he_seal_backend);
      break;
    }
    case OP_TYPEID::BoundedRelu: {
      const auto bounded_relu = static_cast<const op::BoundedRelu*>(&node);
      float alpha = bounded_relu->get_alpha();
      size_t output_size = args[0]->get_batched_element_count();
      if (enable_client()) {
        handle_server_relu_op(args[0], out[0], node);
      } else {
        NGRAPH_WARN << "Performing BoundedRelu without client is not "
                       "privacy-preserving ";
        NGRAPH_CHECK(output_size == args[0]->data().size(), "output size ",
                     output_size, " doesn't match number of elements",
                     out[0]->data().size());
        bounded_relu_seal(args[0]->data(), out[0]->data(), alpha, output_size,
                          m_he_seal_backend);
      }
      break;
    }
    case OP_TYPEID::Broadcast: {
      const auto broadcast = static_cast<const op::Broadcast*>(&node);
      broadcast_seal(args[0]->data(), out[0]->data(),
                     args[0]->get_packed_shape(), out[0]->get_packed_shape(),
                     broadcast->get_broadcast_axes());
      break;
    }
    case OP_TYPEID::Concat: {
      const auto* concat = static_cast<const op::Concat*>(&node);
      std::vector<Shape> in_shapes;
      std::vector<std::vector<HEType>> in_args;
      for (auto& arg : args) {
        in_args.push_back(arg->data());
        in_shapes.push_back(arg->get_packed_shape());
      }
      concat_seal(in_args, out[0]->data(), in_shapes,
                  out[0]->get_packed_shape(), concat->get_concatenation_axis());
      break;
    }
    case OP_TYPEID::Constant: {
      const auto* constant = static_cast<const op::Constant*>(&node);
      constant_seal(out[0]->data(), type, constant->get_data_ptr(),
                    m_he_seal_backend, out[0]->get_batched_element_count());
      break;
    }
    case OP_TYPEID::Convolution: {
      const auto* c = static_cast<const op::Convolution*>(&node);
      const auto& window_movement_strides = c->get_window_movement_strides();
      const auto& window_dilation_strides = c->get_window_dilation_strides();
      const auto& padding_below = c->get_padding_below();
      const auto& padding_above = c->get_padding_above();
      const auto& data_dilation_strides = c->get_data_dilation_strides();

      Shape in_shape0 = args[0]->get_packed_shape();
      Shape in_shape1 = args[1]->get_packed_shape();

      if (verbose) {
        NGRAPH_HE_LOG(3) << in_shape0 << " Conv " << in_shape1 << " => "
                         << out[0]->get_packed_shape();
      }
      convolution_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                       in_shape0, in_shape1, out[0]->get_packed_shape(),
                       window_movement_strides, window_dilation_strides,
                       padding_below, padding_above, data_dilation_strides, 0,
                       1, 1, 0, 0, 1, type, batch_size(), m_he_seal_backend,
                       verbose);

      if (m_he_seal_backend.lazy_mod()) {
        mod_reduce_seal(out[0]->data(), m_he_seal_backend, verbose);
      }
      rescale_seal(out[0]->data(), m_he_seal_backend, verbose);

      break;
    }
    case OP_TYPEID::Divide: {
      Shape in_shape0 = args[0]->get_packed_shape();
      Shape in_shape1 = args[1]->get_packed_shape();

      divide_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                  out[0]->get_batched_element_count(), type, m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Dot: {
      const auto* dot = static_cast<const op::Dot*>(&node);

      Shape in_shape0 = args[0]->get_packed_shape();
      Shape in_shape1 = args[1]->get_packed_shape();

      if (verbose) {
        NGRAPH_HE_LOG(3) << in_shape0 << " dot " << in_shape1;
      }
      {
        dot_seal(args[0]->data(), args[1]->data(), out[0]->data(), in_shape0,
                 in_shape1, out[0]->get_packed_shape(),
                 dot->get_reduction_axes_count(), type, batch_size(),
                 m_he_seal_backend);
      }

      if (m_he_seal_backend.lazy_mod()) {
        mod_reduce_seal(out[0]->data(), m_he_seal_backend, verbose);
      }
      rescale_seal(out[0]->data(), m_he_seal_backend, verbose);

      break;
    }
    case OP_TYPEID::Exp: {
      NGRAPH_CHECK(!enable_client(),
                   "Exp not implemented for client-aided model ");
      NGRAPH_WARN
          << " Performing Exp without client is not privacy-preserving ";
      exp_seal(args[0]->data(), out[0]->data(),
               args[0]->get_batched_element_count(), m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Max: {
      const auto* max = static_cast<const op::Max*>(&node);
      auto reduction_axes = max->get_reduction_axes();
      NGRAPH_CHECK(!args[0]->is_packed() ||
                       (reduction_axes.find(0) == reduction_axes.end()),
                   "Max reduction axes cannot contain 0 for packed tensors");
      NGRAPH_CHECK(!enable_client(),
                   "Max not implemented for client-aided model");
      NGRAPH_WARN << "Performing Max without client is not "
                     "privacy-preserving";

      size_t output_size = args[0]->get_batched_element_count();
      NGRAPH_CHECK(output_size == args[0]->data().size(), "output size ",
                   output_size, " doesn't match number of elements",
                   out[0]->data().size());
      max_seal(args[0]->data(), out[0]->data(), args[0]->get_packed_shape(),
               out[0]->get_packed_shape(), max->get_reduction_axes(),
               out[0]->get_batch_size(), m_he_seal_backend);
      break;
    }
    case OP_TYPEID::MaxPool: {
      const auto* max_pool = static_cast<const op::MaxPool*>(&node);
      if (enable_client()) {
        handle_server_max_pool_op(args[0], out[0], node);
      } else {
        NGRAPH_WARN << "Performing MaxPool without client is not "
                       "privacy-preserving";
        size_t output_size = args[0]->get_batched_element_count();
        NGRAPH_CHECK(output_size == args[0]->data().size(), "output size ",
                     output_size, " doesn't match number of elements",
                     out[0]->data().size());
        max_pool_seal(args[0]->data(), out[0]->data(),
                      args[0]->get_packed_shape(), out[0]->get_packed_shape(),
                      max_pool->get_window_shape(),
                      max_pool->get_window_movement_strides(),
                      max_pool->get_padding_below(),
                      max_pool->get_padding_above(), m_he_seal_backend);
      }
      break;
    }
    case OP_TYPEID::Minimum: {
      minimum_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                   out[0]->get_batched_element_count(), m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Multiply: {
      // Avoid lazy mod for single multiply op
      if (m_he_seal_backend.lazy_mod()) {
        m_he_seal_backend.lazy_mod() = false;
        multiply_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                      out[0]->get_batched_element_count(), type,
                      m_he_seal_backend);
        m_he_seal_backend.lazy_mod() = true;
      } else {
        multiply_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                      out[0]->get_batched_element_count(), type,
                      m_he_seal_backend);
      }
      rescale_seal(out[0]->data(), m_he_seal_backend, verbose);
      break;
    }
    case OP_TYPEID::Negative: {
      negate_seal(args[0]->data(), out[0]->data(),
                  out[0]->get_batched_element_count(), type, m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Pad: {
      const auto* pad = static_cast<const op::Pad*>(&node);
      pad_seal(args[0]->data(), args[1]->data(), out[0]->data(),
               args[0]->get_packed_shape(), out[0]->get_packed_shape(),
               pad->get_padding_below(), pad->get_padding_above(),
               pad->get_pad_mode());
      break;
    }
    case OP_TYPEID::Parameter: {
      NGRAPH_HE_LOG(3) << "Skipping parameter";
      break;
    }
    case OP_TYPEID::Power: {
      // TODO(fboemer): implement with client
      NGRAPH_WARN
          << "Performing Power without client is not privacy preserving ";

      power_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                 out[0]->data().size(), type, m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Relu: {
      if (enable_client()) {
        handle_server_relu_op(args[0], out[0], node);
      } else {
        NGRAPH_WARN << "Performing Relu without client is not privacy "
                       "preserving ";
        size_t output_size = args[0]->get_batched_element_count();
        NGRAPH_CHECK(output_size == args[0]->data().size(), "output size ",
                     output_size, "doesn't match number of elements",
                     out[0]->data().size());
        relu_seal(args[0]->data(), out[0]->data(), output_size,
                  m_he_seal_backend);
      }
      break;
    }
    case OP_TYPEID::Reshape: {
      const auto* reshape = static_cast<const op::Reshape*>(&node);
      if (verbose) {
        NGRAPH_HE_LOG(3) << args[0]->get_packed_shape() << " reshape "
                         << out[0]->get_packed_shape();
      }
      reshape_seal(args[0]->data(), out[0]->data(), args[0]->get_packed_shape(),
                   reshape->get_input_order(), out[0]->get_packed_shape());

      break;
    }
    case OP_TYPEID::Result: {
      result_seal(args[0]->data(), out[0]->data(),
                  out[0]->get_batched_element_count(), m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Reverse: {
      const auto* reverse = static_cast<const op::Reverse*>(&node);
      if (verbose) {
        NGRAPH_HE_LOG(3) << args[0]->get_packed_shape() << " reshape "
                         << out[0]->get_packed_shape();
      }
      reverse_seal(args[0]->data(), out[0]->data(), args[0]->get_packed_shape(),
                   out[0]->get_packed_shape(), reverse->get_reversed_axes());
      break;
    }
    case OP_TYPEID::Slice: {
      const auto* slice = static_cast<const op::Slice*>(&node);
      const Shape& in_shape = args[0]->get_packed_shape();
      const Shape& out_shape = out[0]->get_packed_shape();
      const Coordinate& lower_bounds = slice->get_lower_bounds();
      Coordinate upper_bounds = slice->get_upper_bounds();
      const Strides& strides = slice->get_strides();

      if (verbose) {
        NGRAPH_HE_LOG(3) << "in_shape " << in_shape;
        NGRAPH_HE_LOG(3) << "out_shape " << out_shape;
        NGRAPH_HE_LOG(3) << "lower_bounds " << lower_bounds;
        NGRAPH_HE_LOG(3) << "upper_bounds " << upper_bounds;
        NGRAPH_HE_LOG(3) << "strides " << strides;
      }

      if (!upper_bounds.empty() && !upper_bounds.empty() &&
          (upper_bounds[0] > in_shape[0])) {
        NGRAPH_CHECK(upper_bounds[0] == out[0]->get_batch_size(),
                     "Slice upper bound shape ", upper_bounds,
                     " is not compatible with tensor output shape ",
                     out[0]->get_shape());
        upper_bounds[0] = 1;
        if (verbose) {
          NGRAPH_HE_LOG(3) << "new upper_bounds " << upper_bounds;
        }
      }

      slice_seal(args[0]->data(), out[0]->data(), in_shape, lower_bounds,
                 upper_bounds, strides, out_shape);

      break;
    }
    case OP_TYPEID::Softmax: {
      const auto* softmax = static_cast<const op::Softmax*>(&node);
      auto axes = softmax->get_axes();
      NGRAPH_CHECK(!args[0]->is_packed() || (axes.find(0) == axes.end()),
                   "Softmax axes cannot contain 0 for packed tensors");

      softmax_seal(args[0]->data(), out[0]->data(), args[0]->get_packed_shape(),
                   axes, type, m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Subtract: {
      subtract_seal(args[0]->data(), args[1]->data(), out[0]->data(),
                    out[0]->get_batched_element_count(), type,
                    m_he_seal_backend);
      break;
    }
    case OP_TYPEID::Sum: {
      const auto* sum = static_cast<const op::Sum*>(&node);
      sum_seal(args[0]->data(), out[0]->data(), args[0]->get_packed_shape(),
               out[0]->get_packed_shape(), sum->get_reduction_axes(), type,
               m_he_seal_backend);
      break;
    }
    // Unsupported ops
    case OP_TYPEID::Abs:
    case OP_TYPEID::Acos:
    case OP_TYPEID::All:
    case OP_TYPEID::AllReduce:
    case OP_TYPEID::And:
    case OP_TYPEID::Any:
    case OP_TYPEID::ArgMax:
    case OP_TYPEID::ArgMin:
    case OP_TYPEID::Asin:
    case OP_TYPEID::Atan:
    case OP_TYPEID::Atan2:
    case OP_TYPEID::AvgPoolBackprop:
    case OP_TYPEID::BatchMatMul:
    case OP_TYPEID::BatchMatMulTranspose:
    case OP_TYPEID::BatchNormTraining:
    case OP_TYPEID::BatchNormTrainingBackprop:
    case OP_TYPEID::BroadcastDistributed:
    case OP_TYPEID::BroadcastLike:
    case OP_TYPEID::Ceiling:
    case OP_TYPEID::Clamp:
    case OP_TYPEID::Convert:
    case OP_TYPEID::ConvolutionBackpropData:
    case OP_TYPEID::ConvolutionBackpropFilters:
    case OP_TYPEID::ConvolutionBias:
    case OP_TYPEID::ConvolutionBiasAdd:
    case OP_TYPEID::ConvolutionBiasBackpropFiltersBias:
    case OP_TYPEID::Cos:
    case OP_TYPEID::Cosh:
    case OP_TYPEID::CrossEntropy:
    case OP_TYPEID::CrossEntropyBackprop:
    case OP_TYPEID::CropAndResize:
    case OP_TYPEID::CumSum:
    case OP_TYPEID::DepthToSpace:
    case OP_TYPEID::Dequantize:
    case OP_TYPEID::DynBroadcast:
    case OP_TYPEID::DynPad:
    case OP_TYPEID::DynReshape:
    case OP_TYPEID::DynSlice:
    case OP_TYPEID::DynReplaceSlice:
    case OP_TYPEID::Elu:
    case OP_TYPEID::EmbeddingLookup:
    case OP_TYPEID::Equal:
    case OP_TYPEID::Erf:
    case OP_TYPEID::FakeQuantize:
    case OP_TYPEID::Floor:
    case OP_TYPEID::Gather:
    case OP_TYPEID::GatherND:
    case OP_TYPEID::GenerateMask:
    case OP_TYPEID::GetOutputElement:
    case OP_TYPEID::Gelu:
    case OP_TYPEID::Gemm:
    case OP_TYPEID::GroupConvolution:
    case OP_TYPEID::GroupConvolutionBackpropData:
    case OP_TYPEID::GroupConvolutionBackpropFilters:
    case OP_TYPEID::GroupConvolutionTranspose:
    case OP_TYPEID::GeluBackpropFactor:
    case OP_TYPEID::Greater:
    case OP_TYPEID::GreaterEq:
    case OP_TYPEID::GRN:
    case OP_TYPEID::GRUCell:
    case OP_TYPEID::HardSigmoid:
    case OP_TYPEID::Interpolate:
    case OP_TYPEID::LayerNorm:
    case OP_TYPEID::LayerNormBackprop:
    case OP_TYPEID::Less:
    case OP_TYPEID::LessEq:
    case OP_TYPEID::Log:
    case OP_TYPEID::LRN:
    case OP_TYPEID::LSTMCell:
    case OP_TYPEID::LSTMSequence:
    case OP_TYPEID::Maximum:
    case OP_TYPEID::MatMul:
    case OP_TYPEID::MaxPoolBackprop:
    case OP_TYPEID::MVN:
    case OP_TYPEID::Min:
    case OP_TYPEID::NormalizeL2:
    case OP_TYPEID::Not:
    case OP_TYPEID::NotEqual:
    case OP_TYPEID::OneHot:
    case OP_TYPEID::Or:
    case OP_TYPEID::Passthrough:
    case OP_TYPEID::PRelu:
    case OP_TYPEID::PartialSlice:
    case OP_TYPEID::PartialSliceBackprop:
    case OP_TYPEID::Product:
    case OP_TYPEID::Quantize:
    case OP_TYPEID::QuantizedConvolutionBias:
    case OP_TYPEID::QuantizedConvolutionBiasAdd:
    case OP_TYPEID::QuantizedConvolutionBiasSignedAdd:
    case OP_TYPEID::QuantizedConvolutionRelu:
    case OP_TYPEID::QuantizedConvolution:
    case OP_TYPEID::QuantizedDot:
    case OP_TYPEID::QuantizedDotBias:
    case OP_TYPEID::Recv:
    case OP_TYPEID::Range:
    case OP_TYPEID::RandomUniform:
    case OP_TYPEID::ReluBackprop:
    case OP_TYPEID::ReplaceSlice:
    case OP_TYPEID::ReverseSequence:
    case OP_TYPEID::Round:
    case OP_TYPEID::RNNCell:
    case OP_TYPEID::ScalarConstantLike:
    case OP_TYPEID::ScaleShift:
    case OP_TYPEID::ScatterAdd:
    case OP_TYPEID::ScatterND:
    case OP_TYPEID::ScatterNDAdd:
    case OP_TYPEID::ShapeOf:
    case OP_TYPEID::Send:
    case OP_TYPEID::Select:
    case OP_TYPEID::Selu:
    case OP_TYPEID::ShuffleChannels:
    case OP_TYPEID::Sigmoid:
    case OP_TYPEID::SigmoidBackprop:
    case OP_TYPEID::Sign:
    case OP_TYPEID::Sin:
    case OP_TYPEID::Sinh:
    case OP_TYPEID::SoftmaxCrossEntropy:
    case OP_TYPEID::SoftmaxCrossEntropyBackprop:
    case OP_TYPEID::SpaceToDepth:
    case OP_TYPEID::Split:
    case OP_TYPEID::SquaredDifference:
    case OP_TYPEID::Squeeze:
    case OP_TYPEID::Sqrt:
    case OP_TYPEID::Stack:
    case OP_TYPEID::StopGradient:
    case OP_TYPEID::Tan:
    case OP_TYPEID::Tanh:
    case OP_TYPEID::TensorIterator:
    case OP_TYPEID::Tile:
    case OP_TYPEID::TopK:
    case OP_TYPEID::Unsqueeze:
    case OP_TYPEID::Xor:
    case OP_TYPEID::UnknownOp:
      throw unsupported_op("Unsupported op '" + node.description() + "'");
#pragma clang diagnostic pop
  }
}  // namespace ngraph::runtime::he

void HESealExecutable::handle_server_max_pool_op(
    const std::shared_ptr<HETensor>& arg, const std::shared_ptr<HETensor>& out,
    const Node& node) {
  NGRAPH_HE_LOG(3) << "Server handle_server_max_pool_op";

  bool verbose = verbose_op(&node);
  const auto* max_pool = static_cast<const op::MaxPool*>(&node);

  m_max_pool_done = false;

  Shape unpacked_arg_shape = node.get_input_shape(0);
  Shape out_shape = HETensor::pack_shape(node.get_output_shape(0));

  // TODO(fboemer): call max_pool_seal directly?
  std::vector<std::vector<size_t>> maximize_lists = max_pool_seal_max_list(
      unpacked_arg_shape, out_shape, max_pool->get_window_shape(),
      max_pool->get_window_movement_strides(), max_pool->get_padding_below(),
      max_pool->get_padding_above());

  m_max_pool_data.clear();

  for (const auto& maximize_list : maximize_lists) {
    pb::TCPMessage pb_message;
    pb_message.set_type(pb::TCPMessage_Type_REQUEST);

    json js = {{"function", node.description()}};
    pb::Function f;
    f.set_function(js.dump());
    *pb_message.mutable_function() = f;

    std::vector<HEType> cipher_batch;
    cipher_batch.reserve(maximize_list.size());
    for (const size_t max_ind : maximize_list) {
      cipher_batch.emplace_back(arg->data(max_ind));
    }

    NGRAPH_CHECK(!cipher_batch.empty(), "Maxpool cipher batch is empty");

    HETensor max_pool_tensor(
        arg->get_element_type(),
        Shape{cipher_batch[0].batch_size(), cipher_batch.size()},
        cipher_batch[0].plaintext_packing(), cipher_batch[0].complex_packing(),
        true, m_he_seal_backend);
    max_pool_tensor.data() = cipher_batch;
    const auto& pb_tensors = max_pool_tensor.write_to_pb_tensors();
    NGRAPH_CHECK(pb_tensors.size() == 1,
                 "Only support MaxPool with 1 proto tensor");
    *pb_message.add_he_tensors() = pb_tensors[0];

    // Send list of ciphertexts to maximize over to client
    if (verbose) {
      NGRAPH_HE_LOG(3) << "Sending " << cipher_batch.size()
                       << " Maxpool ciphertexts to client";
    }

    m_session->write_message(TCPMessage(std::move(pb_message)));

    // Acquire lock
    std::unique_lock<std::mutex> mlock(m_max_pool_mutex);

    // Wait until max is done
    m_max_pool_cond.wait(mlock,
                         std::bind(&HESealExecutable::max_pool_done, this));

    // Reset for next max_pool call
    m_max_pool_done = false;
  }
  out->data() = m_max_pool_data;
}

void HESealExecutable::handle_server_relu_op(
    const std::shared_ptr<HETensor>& arg, const std::shared_ptr<HETensor>& out,
    const Node& node) {
  NGRAPH_HE_LOG(3) << "Server handle_server_relu_op"
                   << (enable_garbled_circuits() ? " with garbled circuits"
                                                 : "");

  auto type_id = get_typeid(node.get_type_info());
  NGRAPH_CHECK(type_id == OP_TYPEID::Relu || type_id == OP_TYPEID::BoundedRelu,
               "only support relu / bounded relu");

  bool verbose = verbose_op(&node);
  size_t element_count = arg->data().size();

  size_t smallest_ind =
      match_to_smallest_chain_index(arg->data(), m_he_seal_backend);
  if (verbose) {
    NGRAPH_HE_LOG(3) << "Matched moduli to chain ind " << smallest_ind;
  }

  m_relu_data.resize(element_count, HEType(HEPlaintext(), false));

  // TODO(fboemer): tune
  const size_t max_relu_message_cnt = 1000;

  m_unknown_relu_idx.clear();
  m_unknown_relu_idx.reserve(element_count);

  // Process known values
  for (size_t relu_idx = 0; relu_idx < element_count; ++relu_idx) {
    auto& he_type = arg->data(relu_idx);
    if (he_type.is_plaintext()) {
      m_relu_data[relu_idx].set_plaintext(HEPlaintext());
      if (type_id == OP_TYPEID::Relu) {
        scalar_relu_seal(he_type.get_plaintext(),
                         m_relu_data[relu_idx].get_plaintext());
      } else {
        const auto* bounded_relu = static_cast<const op::BoundedRelu*>(&node);
        float alpha = bounded_relu->get_alpha();
        scalar_bounded_relu_seal(he_type.get_plaintext(),
                                 m_relu_data[relu_idx].get_plaintext(), alpha);
      }
    } else {
      m_unknown_relu_idx.emplace_back(relu_idx);
    }
  }
  auto process_unknown_relu_ciphers_batch = [&](std::vector<HEType>&
                                                    cipher_batch) {
    if (verbose) {
      NGRAPH_HE_LOG(3) << "Sending relu request size " << cipher_batch.size();
    }

    pb::TCPMessage proto_msg;
    proto_msg.set_type(pb::TCPMessage_Type_REQUEST);
    *proto_msg.mutable_function() = node_to_pb_function(
        node,
        {{"enable_gc", bool_to_string(enable_garbled_circuits())},
         {"num_aby_parties",
          std::to_string(m_he_seal_backend.num_garbled_circuit_threads())}});
    std::string function_str = proto_msg.function().function();

    // TODO(fboemer): set complex_packing to correct values?
    auto relu_tensor = std::make_shared<HETensor>(
        arg->get_element_type(),
        Shape{cipher_batch[0].batch_size(), cipher_batch.size()},
        arg->is_packed(), false, true, m_he_seal_backend);
    relu_tensor->data() = cipher_batch;

#ifdef NGRAPH_HE_ABY_ENABLE
    if (enable_garbled_circuits()) {
      // Masks input values
      m_aby_executor->prepare_aby_circuit(function_str, relu_tensor);
    }
#endif

    const auto pb_tensors = relu_tensor->write_to_pb_tensors();
    for (const auto& pb_tensor : pb_tensors) {
      pb::TCPMessage write_msg;
      write_msg.set_type(pb::TCPMessage_Type_REQUEST);
      *write_msg.mutable_function() = node_to_pb_function(
          node,
          {{"enable_gc", bool_to_string(enable_garbled_circuits())},
           {"num_aby_parties",
            std::to_string(m_he_seal_backend.num_garbled_circuit_threads())}});

      *write_msg.add_he_tensors() = pb_tensor;
      TCPMessage relu_message(std::move(write_msg));

      NGRAPH_HE_LOG(5) << "Server writing relu request message";
      m_session->write_message(std::move(relu_message));

#ifdef NGRAPH_HE_ABY_ENABLE
      if (enable_garbled_circuits()) {
        m_aby_executor->run_aby_circuit(function_str, relu_tensor);
      }
#endif
    }
  };

  // Process unknown values
  std::vector<HEType> relu_ciphers_batch;
  relu_ciphers_batch.reserve(max_relu_message_cnt);

  for (const auto& unknown_relu_idx : m_unknown_relu_idx) {
    NGRAPH_CHECK(arg->data(unknown_relu_idx).is_ciphertext(),
                 "HEType should be ciphertext");
    relu_ciphers_batch.emplace_back(arg->data(unknown_relu_idx));
    if (relu_ciphers_batch.size() == max_relu_message_cnt) {
      process_unknown_relu_ciphers_batch(relu_ciphers_batch);
      relu_ciphers_batch.clear();
    }
  }
  if (!relu_ciphers_batch.empty()) {
    process_unknown_relu_ciphers_batch(relu_ciphers_batch);
    relu_ciphers_batch.clear();
  }

  // Wait until all batches have been processed
  std::unique_lock<std::mutex> mlock(m_relu_mutex);
  m_relu_cond.wait(
      mlock, [=]() { return m_relu_done_count == m_unknown_relu_idx.size(); });
  m_relu_done_count = 0;

  out->data() = m_relu_data;
}
}  // namespace ngraph::runtime::he
