/**
 * @file tests/unit/test_cert_chain.cpp
 * @brief Regression coverage for crypto::cert_chain_t::verify.
 *
 * Guards against re-introducing an error-code fast-path in
 * openssl_verify_cb that would accept an unpaired self-signed leaf
 * (GHSA-ph75-mgxh-mv57 / CVE-2026-32253).
 */

#include "../tests_common.h"

#include <src/crypto.h>

TEST(CertChainVerifyTest, RejectsUnpairedSelfSignedLeaf) {
  auto paired = crypto::gen_creds("paired-client", 2048);
  auto attacker = crypto::gen_creds("attacker", 2048);

  crypto::cert_chain_t chain;
  chain.add(crypto::x509(paired.x509));

  auto attacker_x509 = crypto::x509(attacker.x509);
  ASSERT_TRUE(attacker_x509);

  EXPECT_NE(chain.verify(attacker_x509.get()), nullptr);
}

TEST(CertChainVerifyTest, AcceptsPairedSelfSignedLeaf) {
  auto paired = crypto::gen_creds("paired-client", 2048);

  crypto::cert_chain_t chain;
  chain.add(crypto::x509(paired.x509));

  auto presented = crypto::x509(paired.x509);
  ASSERT_TRUE(presented);

  EXPECT_EQ(chain.verify(presented.get()), nullptr);
}
