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

#include <sstream>
#include <unordered_set>

#include "gtest/gtest.h"
#include "he_plaintext.hpp"
#include "he_type.hpp"
#include "logging/ngraph_he_log.hpp"
#include "ngraph/ngraph.hpp"
#include "seal/he_seal_backend.hpp"
#include "seal/seal.h"
#include "seal/seal_ciphertext_wrapper.hpp"
#include "seal/seal_plaintext_wrapper.hpp"
#include "seal/seal_util.hpp"
#include "test_util.hpp"
#include "util/test_tools.hpp"

namespace ngraph::runtime::he {

TEST(seal_util, seal_security_level) {
  EXPECT_EQ(seal_security_level(0), seal::sec_level_type::none);
  EXPECT_EQ(seal_security_level(128), seal::sec_level_type::tc128);
  EXPECT_EQ(seal_security_level(192), seal::sec_level_type::tc192);
  EXPECT_EQ(seal_security_level(256), seal::sec_level_type::tc256);

  EXPECT_ANY_THROW({ seal_security_level(42); });
}

TEST(seal_util, save) {
  seal::EncryptionParameters parms(seal::scheme_type::ckks);
  size_t poly_modulus_degree = 8192;
  parms.set_poly_modulus_degree(poly_modulus_degree);
  parms.set_coeff_modulus(
      seal::CoeffModulus::Create(poly_modulus_degree, {60, 40, 40, 60}));

  std::shared_ptr<seal::SEALContext> context;
  context = std::make_shared<seal::SEALContext>(parms);
  //auto context = seal::SEALContext::Create(parms);

  seal::KeyGenerator keygen(*context);
  seal::PublicKey public_key;
  keygen.create_public_key(public_key);
  auto secret_key = keygen.secret_key();
  seal::RelinKeys relin_keys;
  keygen.create_relin_keys(relin_keys);
  //auto public_key = keygen.public_key();
  //auto secret_key = keygen.secret_key();
  //auto relin_keys = keygen.relin_keys();

  seal::Encryptor encryptor(*context, public_key);
  seal::Evaluator evaluator(*context);
  seal::Decryptor decryptor(*context, secret_key);
  seal::CKKSEncoder encoder(*context);

  std::vector<double> input{0.0, 1.1, 2.2, 3.3};

  seal::Plaintext plain;
  double scale = pow(2.0, 60);
  encoder.encode(input, scale, plain);

  seal::Ciphertext cipher;
  encryptor.encrypt(plain, cipher);
  seal::Ciphertext cipher_load;

  auto* buffer =
      reinterpret_cast<std::byte*>(ngraph_malloc(ciphertext_size(cipher)));

  auto t1 = std::chrono::high_resolution_clock::now();
  auto save_size = save(cipher, buffer);
  auto t2 = std::chrono::high_resolution_clock::now();
  load(cipher_load, context, buffer, save_size);
  auto t3 = std::chrono::high_resolution_clock::now();

  NGRAPH_INFO
      << "save time "
      << std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count()
      << "us";
  NGRAPH_INFO
      << "load time "
      << std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()
      << "us";

  EXPECT_EQ(cipher_load.parms_id(), cipher.parms_id());
  EXPECT_EQ(cipher_load.is_ntt_form(), cipher.is_ntt_form());
  EXPECT_EQ(cipher_load.size(), cipher.size());
  EXPECT_EQ(cipher_load.poly_modulus_degree(), cipher.poly_modulus_degree());
  EXPECT_EQ(cipher_load.coeff_modulus_size(), cipher.coeff_modulus_size());
  //EXPECT_EQ(cipher_load.coeff_mod_count(), cipher.coeff_mod_count());
  EXPECT_EQ(cipher_load.scale(), cipher.scale());
  EXPECT_EQ(cipher_load.is_transparent(), cipher.is_transparent());

  for (size_t i = 0; i < cipher.dyn_array().size(); ++i) {
    EXPECT_EQ(cipher_load[i], cipher[i]);
  }
  ngraph_free(buffer);
}

TEST(seal_util, match_modulus_and_scale_inplace) {
  enum class modulus_operation { None, Rescale, ModSwitch };

  auto test_match_modulus_and_rescale = [&](modulus_operation arg1_op,
                                            modulus_operation arg2_op,
                                            bool reverse_args) {
    auto backend = runtime::Backend::create("HE_SEAL");
    auto he_backend = static_cast<HESealBackend*>(backend.get());
    HEPlaintext plain{1, 2, 3};
    bool complex_packing = false;

    auto cipher1 = HESealBackend::create_empty_ciphertext();
    auto cipher2 = HESealBackend::create_empty_ciphertext();

    auto context = he_backend->get_context();

    encrypt(cipher1, plain, context->first_parms_id(), element::f32,
            he_backend->get_scale(), *he_backend->get_ckks_encoder(),
            *he_backend->get_encryptor(), complex_packing);

    encrypt(cipher2, plain, context->first_parms_id(), element::f32,
            he_backend->get_scale(), *he_backend->get_ckks_encoder(),
            *he_backend->get_encryptor(), complex_packing);

    auto perform_modulus_operation = [&](modulus_operation op,
                                         seal::Ciphertext& ciphertext) {
      if (op == modulus_operation::Rescale) {
        he_backend->get_evaluator()->rescale_to_next_inplace(ciphertext);
      } else if (op == modulus_operation::ModSwitch) {
        he_backend->get_evaluator()->mod_switch_to_next_inplace(ciphertext);
      }
    };

    perform_modulus_operation(arg1_op, cipher1->ciphertext());
    perform_modulus_operation(arg2_op, cipher2->ciphertext());

    if (reverse_args) {
      match_modulus_and_scale_inplace(*cipher2, *cipher1, *he_backend);

    } else {
      match_modulus_and_scale_inplace(*cipher1, *cipher2, *he_backend);
    }

    auto check_decryption = [&](SealCiphertextWrapper& cipher) {
      HEPlaintext output;
      decrypt(output, cipher, complex_packing, *he_backend->get_decryptor(),
              *he_backend->get_ckks_encoder(), he_backend->get_context(),
              plain.size());
      EXPECT_TRUE(
          test::all_close(output.as_double_vec(), plain.as_double_vec()));
    };

    check_decryption(*cipher1);
    check_decryption(*cipher2);
  };

  test_match_modulus_and_rescale(modulus_operation::None,
                                 modulus_operation::None, false);
  test_match_modulus_and_rescale(modulus_operation::ModSwitch,
                                 modulus_operation::None, false);
  test_match_modulus_and_rescale(modulus_operation::ModSwitch,
                                 modulus_operation::None, true);

  // TODO(fboemer): enable tests
  // test_match_modulus_and_rescale(modulus_operation::Rescale,
  //                               modulus_operation::None, false);
  // test_match_modulus_and_rescale(modulus_operation::Rescale,
  //                               modulus_operation::None, true);
}

TEST(seal_util, add_plain_inplace_invalid) {
  auto backend = runtime::Backend::create("HE_SEAL");
  auto he_backend = static_cast<HESealBackend*>(backend.get());
  HEPlaintext plain{1, 2, 3};
  bool complex_packing = false;

  // Encrypted is not valid for encryption parameters
  {
    auto cipher1 = HESealBackend::create_empty_ciphertext();
    EXPECT_ANY_THROW(
        add_plain_inplace(cipher1->ciphertext(), 1.23, *he_backend));
  }
  // Encrypted must be in NTT form
  {
    auto cipher1 = HESealBackend::create_empty_ciphertext();
    auto context = he_backend->get_context();
    encrypt(cipher1, plain, context->first_parms_id(), element::f32,
            he_backend->get_scale(), *he_backend->get_ckks_encoder(),
            *he_backend->get_encryptor(), complex_packing);

    // Falsely set NTT form to false
    cipher1->ciphertext().is_ntt_form() = false;

    EXPECT_ANY_THROW(
        add_plain_inplace(cipher1->ciphertext(), 1.23, *he_backend));
  }
  // Transparent result
  {
    auto cipher1 = HESealBackend::create_empty_ciphertext();
    auto context = he_backend->get_context();
    encrypt(cipher1, HEPlaintext{0, 0, 0}, context->first_parms_id(),
            element::f32, he_backend->get_scale(),
            *he_backend->get_ckks_encoder(), *he_backend->get_encryptor(),
            complex_packing);

    multiply_plain_inplace(cipher1->ciphertext(), 0.00, *he_backend);
    EXPECT_TRUE(cipher1->ciphertext().is_transparent());

    // TODO(fboemer): Multiply plain should also throw error

    // Result would be transparent
    EXPECT_ANY_THROW(
        { add_plain_inplace(cipher1->ciphertext(), 0.00, *he_backend); });
  }
}

TEST(seal_util, multiply_plain_inplace_invalid) {
  auto backend = runtime::Backend::create("HE_SEAL");
  auto he_backend = static_cast<HESealBackend*>(backend.get());
  HEPlaintext plain{1, 2, 3};
  bool complex_packing = false;

  // Encrypted metadata is not valid for encryption parameters
  {
    auto cipher1 = HESealBackend::create_empty_ciphertext();
    EXPECT_ANY_THROW(
        multiply_plain_inplace(cipher1->ciphertext(), 1.23, *he_backend));
  }
  // Encrypted must be in NTT form
  {
    auto cipher1 = HESealBackend::create_empty_ciphertext();
    auto context = he_backend->get_context();
    encrypt(cipher1, plain, context->first_parms_id(), element::f32,
            he_backend->get_scale(), *he_backend->get_ckks_encoder(),
            *he_backend->get_encryptor(), complex_packing);

    // Falsely set NTT form to false
    cipher1->ciphertext().is_ntt_form() = false;

    EXPECT_ANY_THROW(
        multiply_plain_inplace(cipher1->ciphertext(), 1.23, *he_backend));
  }
  // Pool is uninitialized
  {
    auto cipher1 = HESealBackend::create_empty_ciphertext();
    auto context = he_backend->get_context();
    encrypt(cipher1, plain, context->first_parms_id(), element::f32,
            he_backend->get_scale(), *he_backend->get_ckks_encoder(),
            *he_backend->get_encryptor(), complex_packing);

    seal::MemoryPoolHandle pool;
    EXPECT_ANY_THROW(
        multiply_plain_inplace(cipher1->ciphertext(), 1.23, *he_backend, pool));
  }
  // Scale out of bounds
  {
    auto new_backend = runtime::Backend::create("HE_SEAL");
    auto new_he_backend = static_cast<HESealBackend*>(new_backend.get());
    std::string config_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 2048,
        "security_level" : 0,
        "coeff_modulus" : [30],
        "scale" : 16777216
    })";
    std::string error_str;
    new_he_backend->set_config({{"encryption_parameters", config_str}},
                               error_str);

    auto cipher1 = HESealBackend::create_empty_ciphertext();
    auto context = new_he_backend->get_context();
    encrypt(cipher1, plain, context->first_parms_id(), element::f32,
            new_he_backend->get_scale(), *new_he_backend->get_ckks_encoder(),
            *new_he_backend->get_encryptor(), complex_packing);

    EXPECT_ANY_THROW(
        multiply_plain_inplace(cipher1->ciphertext(), 1.23, *new_he_backend));
  }
}

TEST(seal_util, multiply_plain_inplace_large_coeff) {
  auto backend = runtime::Backend::create("HE_SEAL");
  auto he_backend = static_cast<HESealBackend*>(backend.get());
  HEPlaintext plain{1, 2, 3};
  bool complex_packing = false;

  std::string param_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 2048,
        "security_level" : 0,
        "coeff_modulus" : [60, 60],
        "scale" : 16777216
    })";
  std::string error_str;
  he_backend->set_config({{"encryption_parameters", param_str}}, error_str);

  auto cipher1 = HESealBackend::create_empty_ciphertext();
  auto context = he_backend->get_context();
  encrypt(cipher1, plain, context->first_parms_id(), element::f32,
          he_backend->get_scale(), *he_backend->get_ckks_encoder(),
          *he_backend->get_encryptor(), complex_packing);

  multiply_plain_inplace(cipher1->ciphertext(), 1.23, *he_backend);
}

TEST(seal_util, match_to_smallest_chain_index) {
  auto backend = runtime::Backend::create("HE_SEAL");
  auto he_backend = static_cast<HESealBackend*>(backend.get());

  HEPlaintext plain{1, 2, 3};
  size_t vec_size{5};

  std::vector<HEType> plains(vec_size, HEType(plain, false));

  EXPECT_EQ(std::numeric_limits<size_t>::max(),
            match_to_smallest_chain_index(plains, *he_backend));

  EXPECT_EQ(plains.size(), vec_size);
  for (const auto& elem : plains) {
    EXPECT_TRUE(elem.is_plaintext());
    EXPECT_TRUE(test::all_close(elem.get_plaintext().as_double_vec(),
                                plain.as_double_vec()));
  }
}

TEST(seal_util, encode_invalid) {
  auto backend = runtime::Backend::create("HE_SEAL");
  auto he_backend = static_cast<HESealBackend*>(backend.get());
  auto context = he_backend->get_context();

  // Pool is uninitialized
  {
    std::vector<std::uint64_t> dst;
    auto parms_id = context->first_parms_id();
    seal::MemoryPoolHandle pool;
    EXPECT_ANY_THROW(
        encode(1.23, element::f32, 1 << 24U, parms_id, dst, *he_backend, pool));
  }
  // Incorrect scale
  {
    std::vector<std::uint64_t> dst;
    auto parms_id = context->first_parms_id();
    EXPECT_ANY_THROW(
        encode(1.23, element::f32, -1.0, parms_id, dst, *he_backend));
  }
  // Encoded value is too large
  {
    std::vector<std::uint64_t> dst;
    auto parms_id = context->first_parms_id();

#ifdef NGRAPH_HE_ABY_ENABLE
    EXPECT_NO_THROW(encode(std::numeric_limits<float>::max(), element::f32,
                           1 << 29, parms_id, dst, *he_backend));
#else
    EXPECT_ANY_THROW(encode(std::numeric_limits<float>::max(), element::f32,
                            1 << 29, parms_id, dst, *he_backend));
#endif
  }
}

TEST(seal_util, encode) {
  auto test_encode = [](const std::string& param_str, double value,
                        double scale) {
    auto backend = runtime::Backend::create("HE_SEAL");
    auto he_backend = static_cast<HESealBackend*>(backend.get());

    std::string error_str;
    he_backend->set_config({{"encryption_parameters", param_str}}, error_str);

    seal::Plaintext seal_plain;
    std::vector<double> vals(he_backend->get_ckks_encoder()->slot_count(),
                             value);
    he_backend->get_ckks_encoder()->encode(vals, scale, seal_plain);

    auto context = he_backend->get_context();
    std::vector<std::uint64_t> dst;
    auto parms_id = context->first_parms_id();
    encode(value, element::f32, scale, parms_id, dst, *he_backend);

    size_t poly_modulus_degree =
        he_backend->get_encryption_parameters().poly_modulus_degree();

    size_t expected_size = seal_plain.dyn_array().size() / poly_modulus_degree;

    EXPECT_EQ(dst.size(), expected_size);
    for (size_t dst_idx = 0; dst_idx < dst.size(); ++dst_idx) {
      EXPECT_EQ(dst[dst_idx],
                seal_plain.dyn_array()[dst_idx * poly_modulus_degree]);
    }
  };

  // positive value < 60 bits
  {
    double val = pow(2.0, 10);
    double scale = pow(2.0, 40);

    std::string param_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 1024,
        "security_level" : 0,
        "coeff_modulus" : [60, 60, 60],
        "scale" : 1099511627776
    })";
    test_encode(param_str, val, scale);
  }
  // negative value < 60 bits
  {
    double val = -pow(2.0, 10);
    double scale = pow(2.0, 40);

    std::string param_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 1024,
        "security_level" : 0,
        "coeff_modulus" : [60, 60, 60],
        "scale" : 1099511627776
    })";
    test_encode(param_str, val, scale);
  }
  // positive value between 60 and 128 bits
  {
    double val = pow(2.0, 30);
    double scale = pow(2.0, 40);

    std::string param_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 1024,
        "security_level" : 0,
        "coeff_modulus" : [60, 60, 60],
        "scale" : 1099511627776
    })";
    test_encode(param_str, val, scale);
  }
  // negative value between 60 and 128 bits
  {
    double val = -pow(2.0, 30);
    double scale = pow(2.0, 40);

    std::string param_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 1024,
        "security_level" : 0,
        "coeff_modulus" : [60, 60, 60],
        "scale" : 1099511627776
    })";
    test_encode(param_str, val, scale);
  }
  // positive value > 128 bits
  {
    double val = pow(2.0, 63);
    double scale = pow(2.0, 64);

    std::string param_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 1024,
        "security_level" : 0,
        "coeff_modulus" : [60, 60, 60, 60],
        "scale" : 18446744073709551616
    })";
    test_encode(param_str, val, scale);
  }
  // negative value > 128 bits
  {
    double val = -pow(2.0, 63);
    double scale = pow(2.0, 64);

    std::string param_str = R"(
    {
        "scheme_name" : "HE_SEAL",
        "poly_modulus_degree" : 1024,
        "security_level" : 0,
        "coeff_modulus" : [60, 60, 60, 60],
        "scale" : 18446744073709551616
    })";
    test_encode(param_str, val, scale);
  }
}

TEST(seal_util, encode_plaintext_wrapper_wrong_element_type) {
  auto backend = runtime::Backend::create("HE_SEAL");
  auto he_backend = static_cast<HESealBackend*>(backend.get());

  SealPlaintextWrapper plain_wrapper;
  HEPlaintext plain{1, 2, 3};

  auto context = he_backend->get_context();
  EXPECT_ANY_THROW(encode(plain_wrapper, plain, *he_backend->get_ckks_encoder(),
                          context->first_parms_id(), element::i8, 1 << 24,
                          false));
}

}  // namespace ngraph::runtime::he
