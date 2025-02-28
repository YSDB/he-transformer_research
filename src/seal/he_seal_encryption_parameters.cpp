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

#include "seal/he_seal_encryption_parameters.hpp"

#include <exception>
#include <unordered_set>
#include <utility>

#include "logging/ngraph_he_log.hpp"
#include "ngraph/check.hpp"
#include "ngraph/except.hpp"
#include "ngraph/file_util.hpp"
#include "nlohmann/json.hpp"
#include "seal/seal_util.hpp"

namespace ngraph::runtime::he {

HESealEncryptionParameters::HESealEncryptionParameters() {
  auto default_parms = default_real_packing_parms();

  m_scheme_name = default_parms.scheme_name();
  m_seal_encryption_parameters = default_parms.seal_encryption_parameters();
  m_security_level = default_parms.security_level();
  m_scale = default_parms.scale();
  m_complex_packing = default_parms.complex_packing();
}

HESealEncryptionParameters::HESealEncryptionParameters(
    std::string scheme_name, seal::EncryptionParameters parms,
    std::uint64_t security_level, double scale, bool complex_packing)
    : m_scheme_name(std::move(scheme_name)),
      m_seal_encryption_parameters(std::move(parms)),
      m_security_level(security_level),
      m_scale(scale),
      m_complex_packing(complex_packing) {
  validate_parameters();
}

HESealEncryptionParameters
HESealEncryptionParameters::default_real_packing_parms() {
  return HESealEncryptionParameters(
      "HE_SEAL", 1024, std::vector<int>{30, 30, 30, 30, 30}, 0, 1 << 30, false);
}

HESealEncryptionParameters
HESealEncryptionParameters::default_complex_packing_parms() {
  auto real_parms = default_real_packing_parms();
  real_parms.complex_packing() = true;
  return real_parms;
}

HESealEncryptionParameters::HESealEncryptionParameters(
    std::string scheme_name, std::uint64_t poly_modulus_degree,
    std::vector<int> coeff_modulus_bits, std::uint64_t security_level,
    double scale, bool complex_packing)
    : m_scheme_name(std::move(scheme_name)),
      m_security_level(security_level),
      m_scale(scale),
      m_complex_packing(complex_packing) {
  m_seal_encryption_parameters =
      seal::EncryptionParameters(seal::scheme_type::ckks);

  m_seal_encryption_parameters.set_poly_modulus_degree(poly_modulus_degree);

  auto coeff_modulus = seal::CoeffModulus::Create(
      poly_modulus_degree, std::move(coeff_modulus_bits));

  m_seal_encryption_parameters.set_coeff_modulus(coeff_modulus);

  validate_parameters();
}

void HESealEncryptionParameters::validate_parameters() const {
  NGRAPH_CHECK(m_scheme_name == "HE_SEAL", "Invalid scheme name ",
               m_scheme_name);

  static std::unordered_set<uint64_t> valid_poly_modulus{1024, 2048,  4096,
                                                         8192, 16384, 32768}; //This can be changed later.
  NGRAPH_CHECK(valid_poly_modulus.count(poly_modulus_degree()) != 0,
               "poly_modulus_degree must be 1024, 2048, 4096, 8192, 16384, "
               "32768");

  static std::unordered_set<uint64_t> valid_security_level{0, 128, 192, 256};

  NGRAPH_CHECK(valid_security_level.count(security_level()) != 0,
               "security_level must be 0, 128, 192, 256");

  auto seal_sec_level = seal_security_level(security_level());

  seal::SEALContext context(m_seal_encryption_parameters, true, seal_sec_level);
  //std::shared_ptr<seal::SEALContext>& context = &temp_context;
  //context = std::make_shared<seal::SEALContext>(m_seal_encryption_parameters, true, seal_sec_level);
  
  NGRAPH_CHECK(context.parameters_set(), "Invalid parameters");

  // TODO(fboemer): validate scale is reasonable
}

double HESealEncryptionParameters::choose_scale(
    const std::vector<seal::Modulus>& coeff_moduli) {
  if (coeff_moduli.size() > 2) {
    return static_cast<double>(coeff_moduli[coeff_moduli.size() - 2].value());
  }
  if (coeff_moduli.size() > 1) {
    return static_cast<double>(coeff_moduli.back().value()) / 4096.0;
  }
  // Enable a single multiply
  return sqrt(static_cast<double>(coeff_moduli.back().value() / 256.0));
}

bool HESealEncryptionParameters::operator==(
    const HESealEncryptionParameters& other) const {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
  return (other.m_scheme_name == m_scheme_name) &&
         (other.m_seal_encryption_parameters == m_seal_encryption_parameters) &&
         (other.m_security_level == m_security_level) &&
         (other.m_scale == m_scale) &&
         (other.m_complex_packing == m_complex_packing);
#pragma clang diagnostic pop
}

bool HESealEncryptionParameters::same_context(
    const HESealEncryptionParameters& parms1,
    const HESealEncryptionParameters& parms2) {
  auto p1 = parms1;
  auto p2 = parms2;
  p1.complex_packing() = p2.complex_packing();
  p1.security_level() = p2.security_level();
  p1.set_scale(p2.scale());

  return (p1 == p2);
}
//Save&Load a copy of encryptionparameters to/from stream
void HESealEncryptionParameters::save(std::ostream& stream) const {
  stream.write(reinterpret_cast<const char*>(&m_scale), sizeof(m_scale));
  stream.write(reinterpret_cast<const char*>(&m_complex_packing),
               sizeof(m_complex_packing));
  stream.write(reinterpret_cast<const char*>(&m_security_level),
               sizeof(m_security_level));

  m_seal_encryption_parameters.save(stream);
}

HESealEncryptionParameters HESealEncryptionParameters::load(
    std::istream& stream) {
  double scale;
  stream.read(reinterpret_cast<char*>(&scale), sizeof(scale));

  bool complex_packing;
  stream.read(reinterpret_cast<char*>(&complex_packing),
              sizeof(complex_packing));

  uint64_t security_level;
  stream.read(reinterpret_cast<char*>(&security_level), sizeof(security_level));

  seal::EncryptionParameters seal_encryption_parameters;
  seal_encryption_parameters.load(stream);

  return HESealEncryptionParameters("HE_SEAL", seal_encryption_parameters,
                                    security_level, scale, complex_packing);
}

HESealEncryptionParameters
HESealEncryptionParameters::parse_config_or_use_default(const char* config) {
  if (config == nullptr || strlen(config) == 0) {
    return HESealEncryptionParameters();
  }

  std::string json_config_str = config;
  if (file_util::exists(config)) {
    json_config_str = file_util::read_file_to_string(config);
  }

  try {
    // Parse json
    nlohmann::json js = nlohmann::json::parse(json_config_str);
    std::string scheme_name = js["scheme_name"];

    NGRAPH_CHECK(scheme_name == "HE_SEAL", "Parsed scheme name ", scheme_name,
                 " is not HE_SEAL");

    uint64_t poly_modulus_degree = js["poly_modulus_degree"];
    uint64_t security_level = js["security_level"];

    std::vector<int> coeff_mod_bits = js["coeff_modulus"];

    double scale = 0;  // Use default scale
    if (js.find("scale") == js.end()) {
      scale = choose_scale(
          seal::CoeffModulus::Create(poly_modulus_degree, coeff_mod_bits));
    } else {
      scale = js["scale"];
    }

    bool complex_packing = false;
    if (js.find("complex_packing") != js.end()) {
      complex_packing = js["complex_packing"];
    }

    auto params = HESealEncryptionParameters(scheme_name, poly_modulus_degree,
                                             coeff_mod_bits, security_level,
                                             scale, complex_packing);

    return params;
  } catch (const std::exception& e) {
    std::stringstream ss;
    ss << "Error creating encryption parameter from string " << json_config_str
       << ": " << e.what();
    throw ngraph_error(ss.str());
  }
}

void print_encryption_parameters(const HESealEncryptionParameters& params,
                                 const seal::SEALContext& context) {
  auto& context_data = *context.key_context_data();

  std::stringstream param_ss;

  param_ss << "\n/\n"
           << "| Encryption parameters :\n"
           << "|   scheme: ckks\n"
           << "|   poly_modulus_degree: " << params.poly_modulus_degree()
           << "\n"
           << "|   coeff_modulus size: "
           << context_data.total_coeff_modulus_bit_count() << " (";
  auto coeff_modulus = context_data.parms().coeff_modulus();
  std::size_t coeff_mod_count = coeff_modulus.size();
  for (std::size_t i = 0; i < coeff_mod_count - 1; i++) {
    param_ss << coeff_modulus[i].bit_count() << " + ";
  }
  param_ss << coeff_modulus.back().bit_count() << ") bits\n";
  param_ss << "|   scale : " << params.scale() << "\n";

  if (params.complex_packing()) {
    param_ss << "|   complex_packing: True\n";
  } else {
    param_ss << "|   complex_packing: False\n";
  }

  param_ss << "|   security_level: " << params.security_level() << "\n"
           << "\\";

  NGRAPH_HE_LOG(1) << param_ss.str();
}

}  // namespace ngraph::runtime::he
