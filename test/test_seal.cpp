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

#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "seal/seal.h"
#include "seal/seal_util.hpp"
#include "util/all_close.hpp"
#include "util/test_control.hpp"
#include "util/test_tools.hpp"

TEST(seal_example, seal_ckks_basics) {
  seal::EncryptionParameters parms(seal::scheme_type::ckks);
  size_t poly_modulus_degree = 8192;
  parms.set_poly_modulus_degree(poly_modulus_degree);
  parms.set_coeff_modulus(
      seal::CoeffModulus::Create(poly_modulus_degree, {60, 40, 40, 60}));

  //seal::SEALContext t_context(parms);
  std::shared_ptr<seal::SEALContext> context;
  context = std::make_shared<seal::SEALContext>(parms);
  // print_parameters(context);

  seal::KeyGenerator keygen(*context);
  seal::PublicKey public_key;
  keygen.create_public_key(public_key);
  //auto public_key = keygen.public_key();
  auto secret_key = keygen.secret_key();
  seal::RelinKeys relin_keys;
  keygen.create_relin_keys(relin_keys);
  //auto relin_keys = keygen.relin_keys();

  seal::Encryptor encryptor(*context, public_key);
  seal::Evaluator evaluator(*context);
  seal::Decryptor decryptor(*context, secret_key);
  seal::CKKSEncoder encoder(*context);

  std::vector<double> input{0.0, 1.1, 2.2, 3.3};

  seal::Plaintext plain;
  double scale = pow(2.0, 40);
  encoder.encode(input, scale, plain);

  seal::Ciphertext encrypted;
  encryptor.encrypt(plain, encrypted);

  evaluator.square_inplace(encrypted);
  evaluator.relinearize_inplace(encrypted, relin_keys);
  decryptor.decrypt(encrypted, plain);
  encoder.decode(plain, input);

  evaluator.mod_switch_to_next_inplace(encrypted);

  decryptor.decrypt(encrypted, plain);

  encoder.decode(plain, input);

  encrypted.scale() *= 3;
  decryptor.decrypt(encrypted, plain);
  encoder.decode(plain, input);
}

TEST(seal_example, seal_ckks_complex_conjugate) {
  seal::EncryptionParameters parms(seal::scheme_type::ckks);
  size_t poly_modulus_degree = 8192;
  parms.set_poly_modulus_degree(poly_modulus_degree);
  parms.set_coeff_modulus(
      seal::CoeffModulus::Create(poly_modulus_degree, {60, 40, 40, 60}));

  std::shared_ptr<seal::SEALContext> context;
  context = std::make_shared<seal::SEALContext>(parms);
  //auto context = seal::SEALContext::Create(parms);
  // print_parameters(context);

  seal::KeyGenerator keygen(*context);
  seal::PublicKey public_key;
  keygen.create_public_key(public_key);
  //auto public_key = keygen.public_key();
  auto secret_key = keygen.secret_key();
  seal::RelinKeys relin_keys;
  keygen.create_relin_keys(relin_keys);
  //auto relin_keys = keygen.relin_keys();
  seal::GaloisKeys galois_keys;
  keygen.create_galois_keys(galois_keys);
  //auto galois_keys = keygen.galois_keys();

  seal::Encryptor encryptor(*context, public_key);
  seal::Evaluator evaluator(*context);
  seal::Decryptor decryptor(*context, secret_key);
  seal::CKKSEncoder encoder(*context);

  std::vector<std::complex<double>> input{{0.0, 1.1}, {2.2, 3.3}};
  std::vector<std::complex<double>> exp_output{{0.0, -1.1}, {2.2, -3.3}};
  std::vector<std::complex<double>> output;

  seal::Plaintext plain;
  double scale = pow(2.0, 40);
  encoder.encode(input, scale, plain);

  seal::Ciphertext encrypted;
  encryptor.encrypt(plain, encrypted);
  evaluator.complex_conjugate_inplace(encrypted, galois_keys);

  decryptor.decrypt(encrypted, plain);
  encoder.decode(plain, output);

  EXPECT_TRUE(abs(exp_output[0] - output[0]) < 0.1);
  EXPECT_TRUE(abs(exp_output[1] - output[1]) < 0.1);
}
