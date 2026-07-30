/**
 * @file tests/unit/test_crypto_ownership.cpp
 * @brief Ownership discipline around util::uniq_ptr and crypto::signature.
 *
 * Regression coverage for the crash that killed the host twice on
 * 2026-07-29 (minidumps luminalshine-20260729-125516 and -154658): an
 * access violation writing to freed memory inside X509_free's reference
 * count, reached from nvhttp's TLS verify callback.
 *
 * The cause was not in nvhttp. `crypto::signature()` took its certificate
 * as `const crypto::x509_t &` — an OWNING smart pointer — while callers
 * passed a raw `X509 *`. util::uniq_ptr's converting constructor was
 * implicit, so each such call materialised a temporary owner that adopted
 * the caller's pointer and freed it at the end of the full expression.
 * Two distinct failures followed from that one line:
 *
 *   1. The peer certificate was destroyed at the end of every successful
 *      TLS handshake while both OpenSSL's session and nvhttp's
 *      thread_local still referenced it. The next handshake on the same
 *      io_context thread freed the dangling pointer again — the observed
 *      access violation.
 *   2. In the paired-client scan, the borrowed pointer belonged to a live
 *      local `x509_t`, so that certificate was freed by the temporary and
 *      then freed AGAIN when the local went out of scope: an unconditional
 *      double free on every authenticated request, once per stored client.
 *
 * These tests pin both halves of the fix — the non-owning signature and the
 * explicit constructor that makes the whole bug class a compile error — and
 * are pure: no network, no GPU.
 */

// standard includes
#include <string_view>
#include <type_traits>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "../tests_common.h"

#include <src/crypto.h>
#include <src/utility.h>

namespace {

  // --- The systemic guard -------------------------------------------------
  //
  // This is the assertion that matters most. While a raw pointer converted
  // implicitly to an owning uniq_ptr, EVERY function taking a smart pointer
  // by const reference silently accepted `.get()` and freed it. Keeping the
  // conversion explicit is what prevents a fresh instance of this bug from
  // ever compiling again.

  static_assert(
    !std::is_convertible_v<X509 *, crypto::x509_t>,
    "util::uniq_ptr's adopting constructor must stay explicit: an implicit "
    "conversion turns every f(owner.get()) call into a silent free of the "
    "caller's pointer."
  );
  static_assert(
    !std::is_convertible_v<EVP_PKEY *, crypto::pkey_t>,
    "the explicit-adoption rule must hold for every safe_ptr alias, not just x509_t"
  );

  // Adoption must still be possible — just spelled out.
  static_assert(std::is_constructible_v<crypto::x509_t, X509 *>);

  // ...and NOT via assignment either. There is deliberately no adopting
  // `operator=`, so `owner = raw_ptr;` does not compile; reset() is the one
  // way to hand ownership over. (This never compiled before the fix either —
  // the deleted copy-assignment won overload resolution — so nothing in the
  // tree depends on it.)
  static_assert(!std::is_assignable_v<crypto::x509_t &, X509 *>);

  // The nullptr constructor stays implicit, so `x509_t x {};` and passing
  // `nullptr` where an x509_t is expected keep working.
  static_assert(std::is_convertible_v<std::nullptr_t, crypto::x509_t>);

  // --- crypto::signature must not take ownership --------------------------

  static_assert(
    std::is_same_v<decltype(&crypto::signature), std::string_view (*)(const X509 *)>,
    "crypto::signature must take a NON-OWNING const X509 *. Changing it back "
    "to a smart-pointer parameter reintroduces the use-after-free."
  );

  /**
   * A deleter that counts calls, so ownership can be asserted directly
   * rather than inferred from whether the process happens to survive.
   */
  int g_free_count = 0;

  void counting_free(int *p) {
    ++g_free_count;
    delete p;
  }

  using counted_t = util::safe_ptr<int, counting_free>;

  /// Stand-in for a borrowing helper like crypto::signature.
  int borrow_value(const int *p) {
    return p ? *p : -1;
  }

  class CryptoOwnership : public ::testing::Test {
  protected:
    void SetUp() override {
      g_free_count = 0;
    }
  };

  TEST_F(CryptoOwnership, BorrowingHelperDoesNotFreeTheCallersPointer) {
    {
      counted_t owner {new int {7}};
      EXPECT_EQ(borrow_value(owner.get()), 7);

      // The whole point: handing `.get()` to a borrower must not have
      // consumed the object. Under the old implicit constructor an
      // equivalent call against a `const counted_t &` parameter freed it
      // here, and g_free_count would already be 1.
      EXPECT_EQ(g_free_count, 0);
      ASSERT_TRUE(owner);
      EXPECT_EQ(*owner, 7);
    }

    // Exactly one free, performed by the real owner at scope exit.
    EXPECT_EQ(g_free_count, 1);
  }

  TEST_F(CryptoOwnership, AdoptionFreesExactlyOnce) {
    {
      counted_t owner {new int {3}};
      EXPECT_EQ(g_free_count, 0);
    }
    EXPECT_EQ(g_free_count, 1);
  }

  TEST_F(CryptoOwnership, ResetFreesThePreviousValue) {
    counted_t owner {new int {1}};
    owner.reset(new int {2});  // frees the 1
    EXPECT_EQ(g_free_count, 1);
    ASSERT_TRUE(owner);
    EXPECT_EQ(*owner, 2);

    owner.reset();
    EXPECT_EQ(g_free_count, 2);
    EXPECT_FALSE(owner);
  }

  TEST_F(CryptoOwnership, SelfResetIsANoOp) {
    counted_t owner {new int {5}};

    // Reads as "I still own this". Without the equality guard in reset() it
    // freed the object and stored the freed pointer, then double-freed at
    // scope exit.
    owner.reset(owner.get());
    EXPECT_EQ(g_free_count, 0);
    ASSERT_TRUE(owner);
    EXPECT_EQ(*owner, 5);

    owner.reset();
    EXPECT_EQ(g_free_count, 1);
  }

  // --- Behaviour of the fixed signature() ---------------------------------

  TEST_F(CryptoOwnership, SignatureLeavesTheCertificateUsable) {
    auto creds = crypto::gen_creds("ownership-test", 2048);
    auto cert = crypto::x509(creds.x509);
    ASSERT_TRUE(cert);

    const auto first = crypto::signature(cert.get());
    EXPECT_FALSE(first.empty());

    // The certificate must survive the call. Before the fix the pointer was
    // freed here, so everything below was a use-after-free and the second
    // read could return anything at all.
    ASSERT_TRUE(cert);
    EXPECT_NE(X509_get_subject_name(cert.get()), nullptr);

    const auto second = crypto::signature(cert.get());
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.data(), second.data()) << "the view must alias the certificate, not a copy";
  }

  TEST_F(CryptoOwnership, SignatureMatchesOpensslDirectly) {
    auto creds = crypto::gen_creds("ownership-test", 2048);
    auto cert = crypto::x509(creds.x509);
    ASSERT_TRUE(cert);

    const ASN1_BIT_STRING *asn1 = nullptr;
    X509_get0_signature(&asn1, nullptr, cert.get());
    ASSERT_NE(asn1, nullptr);

    const auto sig = crypto::signature(cert.get());
    ASSERT_EQ(sig.size(), static_cast<std::size_t>(asn1->length));
    EXPECT_EQ(sig.data(), reinterpret_cast<const char *>(asn1->data));
  }

  TEST_F(CryptoOwnership, SignatureIsNullSafe) {
    EXPECT_TRUE(crypto::signature(nullptr).empty());
  }

  /**
   * The paired-client scan in nvhttp compares a presented certificate's
   * signature against every stored certificate's signature, re-parsing each
   * stored PEM into a local owner. That loop is where the double free fired,
   * once per stored client per authenticated request. Repeat the shape here
   * and assert both that the comparison works and that repeating it is
   * stable — with the bug, the second pass read freed heap.
   */
  TEST_F(CryptoOwnership, RepeatedStoredCertificateScanIsStable) {
    auto stored = crypto::gen_creds("stored-client", 2048);
    auto other = crypto::gen_creds("other-client", 2048);

    auto presented = crypto::x509(stored.x509);
    ASSERT_TRUE(presented);
    const auto presented_sig = crypto::signature(presented.get());
    ASSERT_FALSE(presented_sig.empty());

    for (int pass = 0; pass < 3; ++pass) {
      bool matched = false;
      for (const auto &pem : {stored.x509, other.x509}) {
        auto stored_x509 = crypto::x509(pem);
        ASSERT_TRUE(stored_x509) << "pass " << pass;

        const auto stored_sig = crypto::signature(stored_x509.get());
        EXPECT_FALSE(stored_sig.empty()) << "pass " << pass;

        if (stored_sig == presented_sig) {
          matched = true;
        }

        // The local owner is still intact after being borrowed from, so its
        // destructor below is the first and only free.
        ASSERT_TRUE(stored_x509) << "pass " << pass;
      }
      EXPECT_TRUE(matched) << "the stored certificate must match itself on pass " << pass;

      // The presented certificate outlives every iteration, so its borrowed
      // view is still valid.
      ASSERT_TRUE(presented) << "pass " << pass;
      EXPECT_EQ(crypto::signature(presented.get()), presented_sig) << "pass " << pass;
    }
  }

  TEST_F(CryptoOwnership, DistinctCertificatesHaveDistinctSignatures) {
    auto a = crypto::gen_creds("client-a", 2048);
    auto b = crypto::gen_creds("client-b", 2048);

    auto ca = crypto::x509(a.x509);
    auto cb = crypto::x509(b.x509);
    ASSERT_TRUE(ca);
    ASSERT_TRUE(cb);

    EXPECT_NE(crypto::signature(ca.get()), crypto::signature(cb.get()));
  }

}  // namespace
