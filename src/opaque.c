/*
    @copyright 2018-21, opaque@ctrlc.hu
    This file is part of libopaque.

    libopaque is free software: you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    libopaque is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libopaque. If not, see <http://www.gnu.org/licenses/>.

    This file implements the Opaque protocol as specified by the IRTF CFRG
*/

#include "opaque.h"
#include <arpa/inet.h>
#include "common.h"

#ifndef HAVE_SODIUM_HKDF
#include "aux/crypto_kdf_hkdf_sha512.h"
#endif

#ifdef CFRG_TEST_VEC
    static const uint8_t *OPAQUE_FINALIZE_INFO=NULL;
    #define OPAQUE_FINALIZE_INFO_LEN 0
#else
/**
 * See oprf_Finalize. TODO Change "OPAQUE01" once the RFC publishes.
 */
    static const uint8_t OPAQUE_FINALIZE_INFO[] = "OPAQUE01";
    #define OPAQUE_FINALIZE_INFO_LEN 8
#endif

#define OPAQUE_RWDU_BYTES 64

typedef struct {
  uint8_t nonce[OPAQUE_ENVELOPE_NONCEBYTES];
  uint8_t auth_tag[crypto_auth_hmacsha512_BYTES];
} __attribute((packed)) Opaque_Envelope;

typedef struct {
  uint8_t client_public_key[crypto_scalarmult_BYTES];
  uint8_t masking_key[crypto_hash_sha512_BYTES];
  Opaque_Envelope envelope;
} __attribute((packed)) Opaque_RegistrationRecord;


// user specific record stored at server upon registration
typedef struct {
  uint8_t kU[crypto_core_ristretto255_SCALARBYTES];
  uint8_t skS[crypto_scalarmult_SCALARBYTES];
  Opaque_RegistrationRecord recU;
} __attribute((packed)) Opaque_UserRecord;

typedef struct {
  uint8_t M[crypto_core_ristretto255_BYTES];
  uint8_t nonceU[OPAQUE_NONCE_BYTES];
  uint8_t X_u[crypto_scalarmult_BYTES];
} __attribute((packed)) Opaque_UserSession;

typedef struct {
  uint8_t blind[crypto_core_ristretto255_SCALARBYTES];
  uint8_t x_u[crypto_scalarmult_SCALARBYTES];
  uint8_t nonceU[OPAQUE_NONCE_BYTES];
  uint8_t M[crypto_core_ristretto255_BYTES];
  uint16_t pwdU_len;
  uint8_t pwdU[];
} __attribute((packed)) Opaque_UserSession_Secret;

typedef struct {
  uint8_t Z[crypto_core_ristretto255_BYTES];
  uint8_t masking_nonce[32];
  uint8_t masked_response[crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)];
  uint8_t nonceS[OPAQUE_NONCE_BYTES];
  uint8_t X_s[crypto_scalarmult_BYTES];
  uint8_t auth[crypto_auth_hmacsha512_BYTES];
} __attribute((packed)) Opaque_ServerSession;

typedef struct {
  uint8_t blind[crypto_core_ristretto255_SCALARBYTES];
  uint16_t pwdU_len;
  uint8_t pwdU[];
} Opaque_RegisterUserSec;

typedef struct {
  uint8_t Z[crypto_core_ristretto255_BYTES];
  uint8_t pkS[crypto_scalarmult_BYTES];
} __attribute((packed)) Opaque_RegisterSrvPub;

typedef struct {
  uint8_t skS[crypto_scalarmult_SCALARBYTES];
  uint8_t kU[crypto_core_ristretto255_SCALARBYTES];
} __attribute((packed)) Opaque_RegisterSrvSec;

typedef struct {
  uint8_t sk[OPAQUE_SHARED_SECRETBYTES];
  uint8_t km2[OPAQUE_HMAC_SHA512_KEYSIZE];
  uint8_t km3[OPAQUE_HMAC_SHA512_KEYSIZE];
} __attribute((packed)) Opaque_Keys;

// sodium defines an hmac with 32B as key, opaque as 64
static void opaque_hmacsha512(const uint8_t key[OPAQUE_HMAC_SHA512_KEYSIZE],
                              const uint8_t *authenticated, const size_t auth_len,
                              uint8_t mac[OPAQUE_HMAC_SHA512_SIZE]) {
  crypto_auth_hmacsha512_state st;
  crypto_auth_hmacsha512_init(&st, key, 64);
  crypto_auth_hmacsha512_update(&st, authenticated, auth_len);
  crypto_auth_hmacsha512_final(&st, mac);
}

/**
 * This function generates an OPRF private key.
 *
 * This is the KeyGen OPRF function defined in the RFC:
 * > OPAQUE only requires an OPRF private key. We write (kU, _) = KeyGen() to denote
 * > use of this function for generating secret key kU (and discarding the
 * > corresponding public key).
 *
 * @param [out] kU - the per-user OPRF private key
 */
static void oprf_KeyGen(uint8_t kU[crypto_core_ristretto255_SCALARBYTES]) {
#ifdef CFRG_TEST_VEC
  unsigned char rtest[] = {
    0xc6, 0x04, 0xc7, 0x85, 0xad, 0xa7, 0x0d, 0x77, 0xa5, 0x25, 0x6a, 0xe2,
    0x17, 0x67, 0xde, 0x8c, 0x33, 0x04, 0x11, 0x52, 0x37, 0xd2, 0x62, 0x13,
    0x4f, 0x5e, 0x46, 0xe5, 0x12, 0xcf, 0x8e, 0x03
  };
  unsigned int rtest_len = 32;
  memcpy(kU,rtest,rtest_len);
#else
  crypto_core_ristretto255_scalar_random(kU);
#endif
}

/**
 * This function computes the OPRF output using input x, N, and domain separation
 * tag info.
 *
 * This is the Finalize OPRF function defined in the RFC.
 *
 * @param [in] x - a value used to compute OPRF (for OPAQUE, this is pwdU, the
 * user's password)
 * @param [in] x_len - the length of param x in bytes
 * @param [in] N - a serialized OPRF group element, a byte array of fixed length,
 * an output of oprf_Unblind
 * @param [in] info - a domain separation tag
 * @param [in] info_len - the length of param info in bytes
 * @param [out] y - an OPRF output
 * @return The function returns 0 if everything is correct.
 */
static int oprf_Finalize(const uint8_t *x, const uint16_t x_len,
                         const uint8_t N[crypto_core_ristretto255_BYTES],
                         const uint8_t *info, const uint16_t info_len,
                         uint8_t rwdU[OPAQUE_RWDU_BYTES]) {
  // according to paper: hash(pwd||H0^k)
  // acccording to voprf IRTF CFRG specification: hash(htons(len(pwd))||pwd||
  //                                              htons(len(info))||info||
  //                                              htons(len(H0_k))||H0_k|||
  //                                              htons(len("Finalize-VOPRF08-\x00\x00\x01"))||"Finalize-VOPRF08-\x00\x00\x01")
  crypto_hash_sha512_state state;
  if(-1==sodium_mlock(&state,sizeof state)) {
    return -1;
  }
  crypto_hash_sha512_init(&state);
  // pwd
  uint16_t size=htons(x_len);
  crypto_hash_sha512_update(&state, (uint8_t*) &size, 2);
  crypto_hash_sha512_update(&state, x, x_len);
#ifdef CFRG_TEST_VEC
  dump(x,x_len,"finalize input");
#endif
  // info
  if(info!=NULL && info_len>0) {
    size=htons(info_len);
    crypto_hash_sha512_update(&state, (uint8_t*) &size, 2);
    crypto_hash_sha512_update(&state, info, info_len);
  } else {
    size=0;
    crypto_hash_sha512_update(&state, (uint8_t*) &size, 2);
    crypto_hash_sha512_update(&state, NULL, 0);
  }
  // H0_k
  size=htons(crypto_core_ristretto255_BYTES);
  crypto_hash_sha512_update(&state, (uint8_t*) &size, 2);
  crypto_hash_sha512_update(&state, N, crypto_core_ristretto255_BYTES);
  const uint8_t DST[]="Finalize-VOPRF08-\x00\x00\x01";
  const uint8_t DST_size=sizeof DST -1;
  size=htons(DST_size);
  crypto_hash_sha512_update(&state, (uint8_t*) &size, 2);
  crypto_hash_sha512_update(&state, DST, DST_size);

  // - concat(y, Harden(y, params))
  uint8_t concated[2*crypto_hash_sha512_BYTES];
  uint8_t *y=concated, *hardened=concated+crypto_hash_sha512_BYTES;
  if(-1==sodium_mlock(&concated,sizeof concated)) {
    sodium_munlock(&state, sizeof state);
    return -1;
  }
  crypto_hash_sha512_final(&state, y);
  sodium_munlock(&state, sizeof state);

#ifdef CFRG_TEST_VEC
  dump((uint8_t*) y, crypto_hash_sha512_BYTES, "output ");
#endif
#ifdef TRACE
  dump((uint8_t*) y, crypto_hash_sha512_BYTES, "y ");
#endif

#ifdef CFRG_TEST_VEC
  // testvectors use identity as MHF
  memcpy(hardened, y, crypto_hash_sha512_BYTES);
#else
  // salt - according to the irtf draft this could be all zeroes
  uint8_t salt[crypto_pwhash_SALTBYTES]={0};
  if (crypto_pwhash(hardened, crypto_hash_sha512_BYTES,
                    (const char*) y, sizeof y, salt,
                    crypto_pwhash_OPSLIMIT_INTERACTIVE,
                    crypto_pwhash_MEMLIMIT_INTERACTIVE,
                    crypto_pwhash_ALG_DEFAULT) != 0) {
    /* out of memory */
    sodium_munlock(concated, sizeof(concated));
    return -1;
  }
#endif
#if (defined TRACE|| defined CFRG_TEST_VEC)
  dump(concated, sizeof concated, "concated");
#endif
  crypto_kdf_hkdf_sha512_extract(rwdU, NULL, 0, concated, sizeof concated);
  sodium_munlock(concated, sizeof(concated));

#if (defined TRACE|| defined CFRG_TEST_VEC)
  dump((uint8_t*) rwdU, OPAQUE_RWDU_BYTES, "rwdU ");
#endif

  return 0;
}

/* expand_loop
 10.    b_i = H(strxor(b_0, b_(i - 1)) || I2OSP(i, 1) || DST_prime)
 */
static void expand_loop(const uint8_t *b_0, const uint8_t *b_i, const uint8_t i, const uint8_t *dst_prime, const uint8_t dst_prime_len, uint8_t *b_ii) {
  uint8_t xored[crypto_hash_sha512_BYTES];
  unsigned j;
  for(j=0;j<sizeof xored;j++) xored[j]=b_0[j]^b_i[j];
  // 8.  b_1 = H(b_0 || I2OSP(1, 1) || DST_prime)
  crypto_hash_sha512_state state;
  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, xored, sizeof xored);
  crypto_hash_sha512_update(&state,(uint8_t*) &i, 1);
  crypto_hash_sha512_update(&state, dst_prime, dst_prime_len);
  crypto_hash_sha512_final(&state, b_ii);
}

/*
 * expand_message_xmd(msg, DST, len_in_bytes)
 * as defined by https://github.com/cfrg/draft-irtf-cfrg-hash-to-curve/blob/master/draft-irtf-cfrg-hash-to-curve.md#expand_message_xmd-hashtofield-expand-xmd
 *
 * Parameters:
 * - H, a hash function (see requirements above).
 * - b_in_bytes, b / 8 for b the output size of H in bits.
 *   For example, for b = 256, b_in_bytes = 32.
 * - r_in_bytes, the input block size of H, measured in bytes (see
 *   discussion above). For example, for SHA-256, r_in_bytes = 64.
 *
 * Input:
 * - msg, a byte string.
 * - DST, a byte string of at most 255 bytes.
 *   See below for information on using longer DSTs.
 * - len_in_bytes, the length of the requested output in bytes.
 *
 * Output:
 * - uniform_bytes, a byte string.
 *
 * Steps:
 * 1.  ell = ceil(len_in_bytes / b_in_bytes)
 * 2.  ABORT if ell > 255
 * 3.  DST_prime = DST || I2OSP(len(DST), 1)
 * 4.  Z_pad = I2OSP(0, r_in_bytes)
 * 5.  l_i_b_str = I2OSP(len_in_bytes, 2)
 * 6.  msg_prime = Z_pad || msg || l_i_b_str || I2OSP(0, 1) || DST_prime
 * 7.  b_0 = H(msg_prime)
 * 8.  b_1 = H(b_0 || I2OSP(1, 1) || DST_prime)
 * 9.  for i in (2, ..., ell):
 * 10.    b_i = H(strxor(b_0, b_(i - 1)) || I2OSP(i, 1) || DST_prime)
 * 11. uniform_bytes = b_1 || ... || b_ell
 * 12. return substr(uniform_bytes, 0, len_in_bytes)
 */
static int expand_message_xmd(const uint8_t *msg, const uint8_t msg_len, const uint8_t *dst, const uint8_t dst_len, const uint8_t len_in_bytes, uint8_t *uniform_bytes) {
  // 1.  ell = ceil(len_in_bytes / b_in_bytes)
  const uint8_t ell = (len_in_bytes + crypto_hash_sha512_BYTES-1) / crypto_hash_sha512_BYTES;
#ifdef TRACE
  fprintf(stderr, "ell %d\n", ell);
  dump(msg, msg_len, "msg");
  dump(dst, dst_len, "dst");
#endif

  // 2.  ABORT if ell > 255
  if(ell>255) return -1;
  // 3.  DST_prime = DST || I2OSP(len(DST), 1)
  uint8_t dst_prime[dst_len+1];
  memcpy(dst_prime, dst, dst_len);
  dst_prime[dst_len] = dst_len;
#ifdef TRACE
  dump(dst_prime, sizeof dst_prime, "dst_prime");
#endif
  // 4.  Z_pad = I2OSP(0, r_in_bytes)
  //const uint8_t r_in_bytes = 128; // for sha512
  uint8_t z_pad[128 /*r_in_bytes*/] = {0}; // supress gcc error: variable-sized object may not be initialized
#ifdef TRACE
  dump(z_pad, sizeof z_pad, "z_pad");
#endif
  // 5.  l_i_b_str = I2OSP(len_in_bytes, 2)
  const uint16_t l_i_b = htons(len_in_bytes);
  const uint8_t *l_i_b_str = (uint8_t*) &l_i_b;
  // 6.  msg_prime = Z_pad || msg || l_i_b_str || I2OSP(0, 1) || DST_prime
  uint8_t msg_prime[sizeof z_pad + msg_len + sizeof l_i_b + 1 + sizeof dst_prime],
    *ptr = msg_prime;
  memcpy(ptr, z_pad, sizeof z_pad);
  ptr += sizeof z_pad;
  memcpy(ptr, msg, msg_len);
  ptr += msg_len;
  memcpy(ptr, l_i_b_str, sizeof l_i_b);
  ptr += sizeof l_i_b;
  *ptr = 0;
  ptr++;
  memcpy(ptr, dst_prime, sizeof dst_prime);
#ifdef TRACE
  dump(msg_prime, sizeof msg_prime, "msg_prime");
#endif
  // 7.  b_0 = H(msg_prime)
  uint8_t b_0[crypto_hash_sha512_BYTES];
  crypto_hash_sha512(b_0, msg_prime, sizeof msg_prime);
#ifdef TRACE
  dump(b_0, sizeof b_0, "b_0");
#endif
  // 8.  b_1 = H(b_0 || I2OSP(1, 1) || DST_prime)
  uint8_t b_i[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state state;
  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, b_0, sizeof b_0);
  crypto_hash_sha512_update(&state,(uint8_t*) &"\x01", 1);
  crypto_hash_sha512_update(&state, dst_prime, sizeof dst_prime);
  crypto_hash_sha512_final(&state, b_i);
#ifdef TRACE
  dump(b_i, sizeof b_i, "b_1");
#endif
  // 9.  for i in (2, ..., ell):
  unsigned left = len_in_bytes;
  uint8_t *out = uniform_bytes;
  unsigned clen = (left>sizeof b_i)?sizeof b_i:left;
  memcpy(out, b_i, clen);
  out+=clen;
  left-=clen;
  int i;
  uint8_t b_ii[crypto_hash_sha512_BYTES];
  for(i=2;i<=ell;i+=2) {
    // 11. uniform_bytes = b_1 || ... || b_ell
    // 12. return substr(uniform_bytes, 0, len_in_bytes)
    // 10.    b_i = H(strxor(b_0, b_(i - 1)) || I2OSP(i, 1) || DST_prime)
    expand_loop(b_0, b_i, i, dst_prime, sizeof dst_prime, b_ii);
    clen = (left>sizeof b_ii)?sizeof b_ii:left;
    memcpy(out, b_ii, clen);
    out+=clen;
    left-=clen;
    // unrolled next iteration so we don't have to swap b_i and b_ii
    expand_loop(b_0, b_ii, i+1, dst_prime, sizeof dst_prime, b_i);
    clen = (left>sizeof b_i)?sizeof b_i:left;
    memcpy(out, b_i, clen);
    out+=clen;
    left-=clen;
  }
  return 0;
}

/* hash-to-ristretto255 - as defined by  https://github.com/cfrg/draft-irtf-cfrg-hash-to-curve/blob/master/draft-irtf-cfrg-hash-to-curve.md#hashing-to-ristretto255-appx-ristretto255
 * Steps:
 * -1. context-string = \x0 + htons(1) // contextString = I2OSP(modeBase(==0), 1) || I2OSP(suite.ID(==1), 2)
 * 0. dst="VOPRF06-HashToGroup-" + context-string (==\x00\x00\x01)
 * 1. uniform_bytes = expand_message(msg, DST, 64)
 * 2. P = ristretto255_map(uniform_bytes)
 * 3. return P
 */
static int voprf_hash_to_group(const uint8_t *msg, const uint8_t msg_len, uint8_t p[crypto_core_ristretto255_BYTES]) {
  const uint8_t dst[] = "HashToGroup-VOPRF08-\x00\x00\x01";
  const uint8_t dst_len = (sizeof dst) - 1;
  uint8_t uniform_bytes[crypto_core_ristretto255_HASHBYTES]={0};
  if(0!=expand_message_xmd(msg, msg_len, dst, dst_len, crypto_core_ristretto255_HASHBYTES, uniform_bytes)) return -1;
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(uniform_bytes, sizeof uniform_bytes, "uniform_bytes");
#endif
  crypto_core_ristretto255_from_hash(p, uniform_bytes);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(p, crypto_core_ristretto255_BYTES, "hashed-to-curve");
#endif
  return 0;
}

static int voprf_hash_to_scalar(const uint8_t *msg, const uint8_t msg_len, const uint8_t *dst, const uint8_t dst_len, uint8_t p[crypto_core_ristretto255_SCALARBYTES]) {
  //const uint8_t dst[] = "HashToScalar-VOPRF08-\x00\x00\x01";
  //const uint8_t dst_len = (sizeof dst) - 1;
  uint8_t uniform_bytes[crypto_core_ristretto255_HASHBYTES]={0};
  if(0!=expand_message_xmd(msg, msg_len, dst, dst_len, crypto_core_ristretto255_HASHBYTES, uniform_bytes)) return -1;
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(uniform_bytes, sizeof uniform_bytes, "uniform_bytes");
#endif
  crypto_core_ristretto255_scalar_reduce(p, uniform_bytes);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(p, crypto_core_ristretto255_BYTES, "hashed-to-scalar");
#endif
  return 0;
}

static int prf(const uint8_t *pwdU, const uint16_t pwdU_len,
               const uint8_t kU[crypto_core_ristretto255_SCALARBYTES],
               uint8_t rwdU[OPAQUE_RWDU_BYTES]) {
  // F_k(pwd) = H(pwd, (H0(pwd))^k) for key k ∈ Z_q
  uint8_t H0[crypto_core_ristretto255_BYTES];
  if(0!=sodium_mlock(H0,sizeof H0)) {
    return -1;
  }
  // sets α := (H^0(pw))^r
  if(0!=voprf_hash_to_group(pwdU, pwdU_len, H0)) return -1;
#ifdef TRACE
  dump(H0,sizeof H0, "H0");
#endif

  // H0 ^ k
  uint8_t N[crypto_core_ristretto255_BYTES];
  sodium_mlock(N,sizeof N);
  if (crypto_scalarmult_ristretto255(N, kU, H0) != 0) {
    sodium_munlock(H0,sizeof H0);
    sodium_munlock(N,sizeof N);
    return -1;
  }
  sodium_munlock(H0,sizeof H0);
#ifdef TRACE
  dump(N, sizeof N, "N");
#endif

  // 2. rwdU = Finalize(pwdU, N, "OPAQUE01")
  if(0!=oprf_Finalize(pwdU, pwdU_len, N, OPAQUE_FINALIZE_INFO, OPAQUE_FINALIZE_INFO_LEN, rwdU)) {
    sodium_munlock(N,sizeof N);
    return -1;
  }
  sodium_munlock(N,sizeof N);

  return 0;
}

/**
 * This function converts input x into an element of the OPRF group, randomizes it
 * by some scalar r, producing M, and outputs (r, M).
 *
 * This is the Blind OPRF function defined in the RFC.
 *
 * @param [in] x - the value to blind (for OPAQUE, this is pwdU, the user's
 * password)
 * @param [in] x_len - the length of param x in bytes
 * @param [out] r - an OPRF scalar value used for randomization
 * @param [out] M - a serialized OPRF group element, a byte array of fixed length,
 * the blinded version of x, an input to oprf_Evaluate
 * @return The function returns 0 if everything is correct.
 */
static int oprf_Blind(const uint8_t *x, const uint16_t x_len,
                      uint8_t r[crypto_core_ristretto255_SCALARBYTES],
                      uint8_t M[crypto_core_ristretto255_BYTES]) {
#ifdef CFRG_TEST_VEC
  dump(x, x_len, "input");
#endif
  uint8_t H0[crypto_core_ristretto255_BYTES];
  if(0!=sodium_mlock(H0,sizeof H0)) {
    return -1;
  }
  // sets α := (H^0(pw))^r
  if(0!=voprf_hash_to_group(x, x_len, H0)) return -1;
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(H0,sizeof H0, "H0 ");
#endif

  // U picks r
#ifdef CFRG_TEST_VEC
  static int vecidx=0;
  const unsigned char rtest[2][32] = {{
           // "blind_registration": "c62937d17dc9aa213c9038f84fe8c5bf3d953356db01c4d48acb7cae48e6a504",
                           0xc6, 0x29, 0x37, 0xd1, 0x7d, 0xc9, 0xaa, 0x21,
                           0x3c, 0x90, 0x38, 0xf8, 0x4f, 0xe8, 0xc5, 0xbf,
                           0x3d, 0x95, 0x33, 0x56, 0xdb, 0x01, 0xc4, 0xd4,
                           0x8a, 0xcb, 0x7c, 0xae, 0x48, 0xe6, 0xa5, 0x04},
           // "blind_login": "b5f458822ea11c900ad776e38e29d7be361f75b4d79b55ad74923299bf8d6503",
                          {0xb5, 0xf4, 0x58, 0x82, 0x2e, 0xa1, 0x1c, 0x90,
                           0x0a, 0xd7, 0x76, 0xe3, 0x8e, 0x29, 0xd7, 0xbe,
                           0x36, 0x1f, 0x75, 0xb4, 0xd7, 0x9b, 0x55, 0xad,
                           0x74, 0x92, 0x32, 0x99, 0xbf, 0x8d, 0x65, 0x03
                           }};
  const unsigned int rtest_len = 32;
  memcpy(r,rtest[vecidx++ % 2],rtest_len);
#else
  crypto_core_ristretto255_scalar_random(r);
#endif

#ifdef TRACE
  dump(r, crypto_core_ristretto255_SCALARBYTES, "r");
#endif
  // H^0(pw)^r
  if (crypto_scalarmult_ristretto255(M, r, H0) != 0) {
    sodium_munlock(H0,sizeof H0);
    return -1;
  }
  sodium_munlock(H0,sizeof H0);
#ifdef CFRG_TEST_VEC
  dump(M, crypto_core_ristretto255_BYTES, "blinded");
#endif
#ifdef TRACE
  dump(M, crypto_core_ristretto255_BYTES, "M");
#endif
  return 0;
}

/**
 * This function evaluates input element M using private key k, yielding output
 * element Z.
 *
 * This is the Evaluate OPRF function defined in the RFC.
 *
 * @param [in] k - a private key (for OPAQUE, this is kU, the user's OPRF private
 * key)
 * @param [in] M - a serialized OPRF group element, a byte array of fixed length,
 * an output of oprf_Blind (for OPAQUE, this is the blinded pwdU, the user's
 * password)
 * @param [out] Z - a serialized OPRF group element, a byte array of fixed length,
 * an input to oprf_Unblind
 * @return The function returns 0 if everything is correct.
 */
static int oprf_Evaluate(const uint8_t k[crypto_core_ristretto255_SCALARBYTES],
                         const uint8_t M[crypto_core_ristretto255_BYTES],
                         uint8_t Z[crypto_core_ristretto255_BYTES]) {
#ifdef CFRG_TEST_VEC
  // cheating here, the current draft unecessarily uses a poprf not
  // the oprf evaluate semantics it is planned to switch back to oprf
  // thus this workaround.
  static int vecidx=0;
  const uint8_t z[2][32] = {{
           0x5c, 0x7d, 0x3c, 0x70, 0xcf, 0x74, 0x78, 0xea,
           0xd8, 0x59, 0xbb, 0x87, 0x9b, 0x37, 0xcc, 0xe7,
           0x8b, 0xae, 0xf3, 0xb9, 0xd8, 0x1e, 0x04, 0xf4,
           0xc7, 0x90, 0xce, 0x25, 0xf2, 0x83, 0x0e, 0x2e,},

          {0x1a, 0xf1, 0x1b, 0xe2, 0x9a, 0x90, 0x32, 0x2d,
           0xc1, 0x64, 0x62, 0xd0, 0x86, 0x1b, 0x1e, 0xb6,
           0x17, 0x61, 0x1f, 0xe2, 0xf0, 0x5e, 0x5e, 0x98,
           0x60, 0xc1, 0x64, 0x59, 0x2d, 0x4f, 0x7f, 0x62}};
  memcpy(Z,z[vecidx++ % 2],sizeof z);
  return 0;
#endif

  return crypto_scalarmult_ristretto255(Z, k, M);
}

/**
 * This function removes random scalar r from Z, yielding output N.
 *
 * This is the Unblind OPRF function defined in the RFC.
 *
 * @param [in] r - an OPRF scalar value used for randomization in oprf_Blind
 * @param [in] Z - a serialized OPRF group element, a byte array of fixed length,
 * an output of oprf_Evaluate
 * @param [out] N - a serialized OPRF group element with random scalar r removed,
 * a byte array of fixed length, an input to oprf_Finalize
 * @return The function returns 0 if everything is correct.
 */
static int oprf_Unblind(const uint8_t r[crypto_core_ristretto255_SCALARBYTES],
                        const uint8_t Z[crypto_core_ristretto255_BYTES],
                        uint8_t N[crypto_core_ristretto255_BYTES]) {
#ifdef TRACE
  dump((uint8_t*) r, crypto_core_ristretto255_SCALARBYTES, "r ");
  dump((uint8_t*) Z, crypto_core_ristretto255_BYTES, "Z ");
#endif

  // (a) Checks that β ∈ G ∗ . If not, outputs (abort, sid , ssid ) and halts;
  if(crypto_core_ristretto255_is_valid_point(Z) != 1) return -1;

  // (b) Computes rw := H(pw, β^1/r );
  // invert r = 1/r
  uint8_t ir[crypto_core_ristretto255_SCALARBYTES];
  if(-1==sodium_mlock(ir, sizeof ir)) return -1;
  if (crypto_core_ristretto255_scalar_invert(ir, r) != 0) {
    sodium_munlock(ir, sizeof ir);
    return -1;
  }
#ifdef TRACE
  dump((uint8_t*) ir, sizeof ir, "r^-1 ");
#endif

  // H0 = β^(1/r)
  // beta^(1/r) = h(pwd)^k
  if (crypto_scalarmult_ristretto255(N, ir, Z) != 0) {
    sodium_munlock(ir, sizeof ir);
    return -1;
  }
#ifdef TRACE
  dump((uint8_t*) N, crypto_core_ristretto255_BYTES, "N ");
#endif

  sodium_munlock(ir, sizeof ir);
  return 0;
}

static void hkdf_expand_label(uint8_t* res, const uint8_t secret[crypto_kdf_hkdf_sha512_KEYBYTES], const char *label, const char transcript[crypto_hash_sha512_BYTES], const size_t len) {
  // construct a hkdf label
  // struct {
  //   uint16 length = Length;
  //   opaque label<8..255> = "OPAQUE-" + Label;
  //   opaque context<0..255> = Context;
  // } HkdfLabel;
  const size_t llen = strlen((const char*) label);
  uint8_t hkdflabel[2+2+7/*"OPAQUE-"*/+llen+(transcript!=NULL?crypto_hash_sha512_BYTES:0)];

  *((uint16_t*) hkdflabel)=htons(len);

  uint8_t *ptr=hkdflabel+2;
  *(ptr)=(7+llen);
  ptr+=1;

  memcpy(ptr,"OPAQUE-",7);
  ptr+=7;

  memcpy(ptr,label,llen);
  ptr+=llen;


  if(transcript!=NULL) {
    *(ptr)=crypto_hash_sha512_BYTES;
    ptr+=1;
    memcpy(ptr, transcript, crypto_hash_sha512_BYTES);
  } else {
    *(ptr)=0;
  }

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(hkdflabel, sizeof(hkdflabel), "expanded label");
  if(transcript!=NULL) dump((const uint8_t*) transcript,crypto_hash_sha512_BYTES, "transcript: ");
#endif

  crypto_kdf_hkdf_sha512_expand(res, len, (const char*) hkdflabel, sizeof(hkdflabel), secret);
}

// derive keys according to irtf cfrg draft
static void derive_keys(Opaque_Keys* keys, const uint8_t ikm[crypto_scalarmult_BYTES * 3], const char info[crypto_hash_sha512_BYTES]) {
  uint8_t prk[64];
  sodium_mlock(prk, sizeof prk);
#ifdef TRACE
  dump(ikm, crypto_scalarmult_BYTES*3, "ikm ");
  dump((uint8_t*) info, crypto_hash_sha512_BYTES, "info ");
#endif
  // 1. prk = HKDF-Extract(salt=0, IKM)
  crypto_kdf_hkdf_sha512_extract(prk, NULL, 0, ikm, crypto_scalarmult_BYTES*3);
#ifdef CFRG_TEST_VEC
  dump(prk, sizeof prk, "prk");
#endif

  // 2. handshake_secret = Derive-Secret(., "handshake secret", info)
  uint8_t handshake_secret[OPAQUE_HANDSHAKE_SECRETBYTES];
  sodium_mlock(handshake_secret, sizeof handshake_secret);
  const char handshake_secret_label[]="HandshakeSecret";
  hkdf_expand_label(handshake_secret, prk, handshake_secret_label, info, sizeof(handshake_secret));

  // 3. keys->sk         = Derive-Secret(., "session secret", info)
  const char session_key_label[]="SessionKey";
  hkdf_expand_label(keys->sk, prk, session_key_label, info, OPAQUE_SHARED_SECRETBYTES);
  sodium_munlock(prk,sizeof(prk));

  // 4. Km2 = Derive-Secret(handshake_secret, "ServerMAC", "")
  //Km2 = HKDF-Expand-Label(handshake_secret, "server mac", "", Hash.length)
  const char server_mac_label[]="ServerMAC";
  hkdf_expand_label(keys->km2, handshake_secret, server_mac_label, NULL, OPAQUE_HMAC_SHA512_KEYSIZE);
  // 5. Km3 = Derive-Secret(handshake_secret, "ClientMAC", "")
  //Km3 = HKDF-Expand-Label(handshake_secret, "client mac", "", Hash.length)
  const char client_mac_label[]="ClientMAC";
  hkdf_expand_label(keys->km3, handshake_secret, client_mac_label, NULL, OPAQUE_HMAC_SHA512_KEYSIZE);
  sodium_munlock(handshake_secret, sizeof handshake_secret);
#ifdef TRACE
  dump(keys->sk, OPAQUE_SHARED_SECRETBYTES, "keys->sk");
  dump(keys->km2, OPAQUE_HMAC_SHA512_KEYSIZE, "keys->km2");
  dump(keys->km3, OPAQUE_HMAC_SHA512_KEYSIZE, "keys->km3");
#endif
}

static void fix_ids(const uint8_t pkU[crypto_scalarmult_BYTES],
                    const uint8_t pkS[crypto_scalarmult_BYTES],
                    const Opaque_Ids *ids0,
                    Opaque_Ids *ids) {
  if(ids0->idS==NULL || ids0->idS_len==0) {
    ids->idS=(uint8_t*)pkS;
    ids->idS_len=crypto_scalarmult_BYTES;
  } else {
    ids->idS=ids0->idS;
    ids->idS_len=ids0->idS_len;
  }
  if(ids0->idU==NULL || ids0->idU_len==0) {
    ids->idU=(uint8_t*)pkU;
    ids->idU_len=crypto_scalarmult_BYTES;
  } else {
    ids->idU=ids0->idU;
    ids->idU_len=ids0->idU_len;
  }
}

static void calc_preamble(char preamble[crypto_hash_sha512_BYTES],
                          crypto_hash_sha512_state *state,
                          const uint8_t pkU[crypto_scalarmult_BYTES],
                          const uint8_t pkS[crypto_scalarmult_BYTES],
                          const uint8_t ke1[OPAQUE_USER_SESSION_PUBLIC_LEN],
                          const Opaque_ServerSession *ke2,
                          const uint8_t *ctx, const uint16_t ctx_len,
                          const Opaque_Ids *ids0) {
  crypto_hash_sha512_init(state);

  Opaque_Ids ids;
  fix_ids(pkU, pkS, ids0, &ids);

#ifdef TRACE
  fprintf(stderr,"calc preamble\n");
  dump(ids.idU, ids.idU_len,"idU ");
  dump(ids.idS, ids.idS_len,"idS ");
  //dump(nonceU, OPAQUE_NONCE_BYTES, "nonceU ");
  //dump(nonceS, OPAQUE_NONCE_BYTES, "nonceS ");
#endif

  //1. preamble = hash("RFCXXXX",
  // note the spec it self does not say hash here, but
  // https://github.com/cfrg/draft-irtf-cfrg-opaque/pull/147
  // and later uses all hash this value
  const uint8_t rfc[]="RFCXXXX";
  const uint8_t rfc_len=sizeof rfc -1;
  crypto_hash_sha512_update(state, rfc, rfc_len);

  //                   I2OSP(len(context), 2), context,
  uint16_t len = htons(ctx_len);
  crypto_hash_sha512_update(state, (uint8_t*) &len, 2);
  crypto_hash_sha512_update(state, ctx, ctx_len);

  //                   I2OSP(len(client_identity), 2), client_identity,
  len = htons(ids.idU_len);
  crypto_hash_sha512_update(state, (uint8_t*) &len, 2);
  crypto_hash_sha512_update(state, ids.idU, ids.idU_len);

  //                   ke1,
  crypto_hash_sha512_update(state, ke1, OPAQUE_USER_SESSION_PUBLIC_LEN);

  //                   I2OSP(len(server_identity), 2), server_identity,
  len = htons(ids.idS_len);
  crypto_hash_sha512_update(state, (uint8_t*) &len, 2);
  crypto_hash_sha512_update(state, ids.idS, ids.idS_len);

  //                   ke2.credential_response,
  //                   ke2.AuthResponse.server_nonce, ke2.AuthResponse.server_keyshare)
  //  see type Opaque_ServerSession
  crypto_hash_sha512_update(state, (uint8_t*)ke2,
                            /* credential_response */
                            /*Z*/ crypto_core_ristretto255_BYTES +
                            /*masking_nonce*/ 32+
                            /*masked_response*/ crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)+
                            /*nonceS*/OPAQUE_NONCE_BYTES+
                            /*X_s*/crypto_scalarmult_BYTES);

  crypto_hash_sha512_final(state, (uint8_t *) preamble);
}

// implements server end of triple-dh
static int server_3dh(Opaque_Keys *keys,
               const uint8_t ix[crypto_scalarmult_SCALARBYTES],
               const uint8_t ex[crypto_scalarmult_SCALARBYTES],
               const uint8_t Ip[crypto_scalarmult_BYTES],
               const uint8_t Ep[crypto_scalarmult_BYTES],
               const char preamble[crypto_hash_sha512_BYTES]) {
  uint8_t sec[crypto_scalarmult_BYTES * 3], *ptr = sec;
  sodium_mlock(sec, sizeof sec);

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(ix, crypto_scalarmult_SCALARBYTES, "skS");
  dump(ex, crypto_scalarmult_SCALARBYTES, "ekS");
  dump(Ip, crypto_scalarmult_BYTES, "pkU");
  dump(Ep, crypto_scalarmult_BYTES, "epkU");
#endif

  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ep)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ix,Ep)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ip)) return 1;
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(sec, 96, "3dh s ikm");
#endif

  derive_keys(keys, sec, preamble);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump((uint8_t*) keys, sizeof(Opaque_Keys), "keys ");
#endif

  sodium_munlock(sec,sizeof(sec));
  return 0;
}

// implements user end of triple-dh
static int user_3dh(Opaque_Keys *keys,
             const uint8_t ix[crypto_scalarmult_SCALARBYTES],
             const uint8_t ex[crypto_scalarmult_SCALARBYTES],
             const uint8_t Ip[crypto_scalarmult_BYTES],
             const uint8_t Ep[crypto_scalarmult_BYTES],
             const char preamble[crypto_hash_sha512_BYTES]) {
  uint8_t sec[crypto_scalarmult_BYTES * 3], *ptr = sec;
  sodium_mlock(sec, sizeof sec);

  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ep)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ip)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ix,Ep)) return 1;
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(sec, 96, "3dh u ikm");
#endif

  // and hash for the result SK = f_K(0)
  derive_keys(keys, sec, preamble);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump((uint8_t*) keys, sizeof(Opaque_Keys), "keys ");
#endif

  sodium_munlock(sec,sizeof(sec));
  return 0;
}

static int skU_from_rwd(const uint8_t rwd[OPAQUE_RWDU_BYTES], const uint8_t nonce[OPAQUE_NONCE_BYTES], uint8_t skU[crypto_scalarmult_BYTES]) {
  char info[OPAQUE_NONCE_BYTES+10];
  memcpy(info, nonce, OPAQUE_NONCE_BYTES);
  memcpy(info+OPAQUE_NONCE_BYTES, "PrivateKey", 10);
  uint8_t seed[crypto_core_ristretto255_SCALARBYTES];
  crypto_kdf_hkdf_sha512_expand(seed, crypto_core_ristretto255_SCALARBYTES, info, sizeof info, rwd);

  uint8_t dst[24]="OPAQUE-DeriveAuthKeyPair";
  if(0!=voprf_hash_to_scalar(seed, sizeof seed, dst, sizeof dst, skU)) {
    return -1;
  }
  return 0;
}

static int create_envelope(const uint8_t rwdU[OPAQUE_RWDU_BYTES],
                           const uint8_t server_public_key[crypto_scalarmult_BYTES],
                           const Opaque_Ids *ids,
                           Opaque_Envelope *env,
                           uint8_t client_public_key[crypto_scalarmult_BYTES],
                           uint8_t masking_key[crypto_hash_sha512_BYTES],
                           uint8_t export_key[crypto_hash_sha512_BYTES]) {

  // 1. envelope_nonce = random(Nn)
#ifdef CFRG_TEST_VEC
  // testvector from irtf cfrg
  // "envelope_nonce": "71b8f14b7a1059cdadc414c409064a22cf9e970b0ffc6f1fc6fdd539c4676775"
  const uint8_t nonce[32] = {
         0x71, 0xb8, 0xf1, 0x4b, 0x7a, 0x10, 0x59, 0xcd,
         0xad, 0xc4, 0x14, 0xc4, 0x09, 0x06, 0x4a, 0x22,
         0xcf, 0x9e, 0x97, 0x0b, 0x0f, 0xfc, 0x6f, 0x1f,
         0xc6, 0xfd, 0xd5, 0x39, 0xc4, 0x67, 0x67, 0x75};
  memcpy(env->nonce, nonce, sizeof nonce);
#else
  randombytes(env->nonce, OPAQUE_ENVELOPE_NONCEBYTES);
#endif

  uint8_t concated[OPAQUE_ENVELOPE_NONCEBYTES+10],
    *label = concated+OPAQUE_ENVELOPE_NONCEBYTES;
  memcpy(concated, env->nonce, OPAQUE_ENVELOPE_NONCEBYTES);

  // 2. masking_key = HKDF-Expand(randomized_pwd, "MaskingKey", Nh)
  const uint8_t masking_key_info[10]="MaskingKey";
  crypto_kdf_hkdf_sha512_expand(masking_key, crypto_hash_sha512_BYTES,
                                (const char*) masking_key_info, sizeof masking_key_info,
                                rwdU);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(masking_key_info, sizeof masking_key_info, "masking_key_info");
  dump(rwdU, OPAQUE_RWDU_BYTES, "rwdU");
  dump(masking_key, crypto_hash_sha512_BYTES, "masking_key");
#endif

  // 3. auth_key = HKDF-Expand(randomized_pwd, concat(envelope_nonce, "AuthKey"), Nh)
  uint8_t auth_key[OPAQUE_HMAC_SHA512_KEYSIZE];
  if(-1==sodium_mlock(auth_key, sizeof auth_key)) {
    return -1;
  }
  memcpy(label, "AuthKey", 7);
  crypto_kdf_hkdf_sha512_expand(auth_key, sizeof auth_key,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+7,
                                rwdU);

#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(auth_key,sizeof auth_key, "auth_key ");
#endif

  // 4. export_key = HKDF-Expand(randomized_pwd, concat(envelope_nonce, "ExportKey"), Nh)
  memcpy(label, "ExportKey", 9);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(concated, OPAQUE_ENVELOPE_NONCEBYTES+9, "export_key_info");
#endif
  crypto_kdf_hkdf_sha512_expand(export_key, crypto_hash_sha512_BYTES,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+9,
                                rwdU);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(export_key,crypto_hash_sha512_BYTES, "export_key ");
#endif

  // 5. seed = Expand(randomized_pwd, concat(envelope_nonce, "PrivateKey"), Nseed)
  uint8_t client_secret_key[crypto_scalarmult_SCALARBYTES];
  if(-1==sodium_mlock(client_secret_key, sizeof client_secret_key)) {
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  memcpy(label, "PrivateKey", 10);
  uint8_t seed[crypto_core_ristretto255_SCALARBYTES];
  crypto_kdf_hkdf_sha512_expand(seed, crypto_core_ristretto255_SCALARBYTES,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+10,
                                rwdU);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(client_secret_key, crypto_scalarmult_SCALARBYTES, "client_secret_key");
#endif

  // 6. _, client_public_key = DeriveAuthKeyPair(seed)
  uint8_t dst[24]="OPAQUE-DeriveAuthKeyPair";
  if(0!=voprf_hash_to_scalar(seed, sizeof seed, dst, sizeof dst, client_secret_key)) {
    sodium_munlock(client_secret_key, sizeof client_secret_key);
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  // P_u := g^p_u
  crypto_scalarmult_ristretto255_base(client_public_key, client_secret_key);
  sodium_munlock(client_secret_key, sizeof client_secret_key);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(client_public_key, crypto_scalarmult_BYTES, "client_public_key");
#endif

  // complete ids in case they are NULL and need to be set to the pk[US]
  Opaque_Ids ids_completed;
  if(ids->idU == NULL || ids->idU_len == 0) {
    ids_completed.idU=client_public_key;
    ids_completed.idU_len=crypto_scalarmult_BYTES;
  } else {
    ids_completed.idU=(uint8_t*)ids->idU;
    ids_completed.idU_len=ids->idU_len;
  }
  if(ids->idS == NULL || ids->idS_len == 0) {
    ids_completed.idS=(uint8_t*)server_public_key;
    ids_completed.idS_len=crypto_scalarmult_BYTES;
  } else {
    ids_completed.idS=(uint8_t*)ids->idS;
    ids_completed.idS_len=ids->idS_len;
  }

  uint8_t authenticated[OPAQUE_NONCE_BYTES+
                        crypto_scalarmult_BYTES+
                        ids_completed.idS_len+2+
                        ids_completed.idU_len+2],
         *ptr=authenticated;

  // nonce
  memcpy(ptr, env->nonce, OPAQUE_NONCE_BYTES);
  ptr+=OPAQUE_NONCE_BYTES;
  // server_public_key
  memcpy(ptr, server_public_key, crypto_scalarmult_BYTES);
  ptr+=crypto_scalarmult_BYTES;
  // server_identity
  uint16_t size = htons(ids_completed.idS_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids_completed.idS,ids_completed.idS_len);
  ptr+=ids_completed.idS_len;
  // client_identity
  size = htons(ids_completed.idU_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids_completed.idU,ids_completed.idU_len);

  opaque_hmacsha512(auth_key,             // key
                    authenticated,        // in
                    sizeof authenticated, // len(in)
                    env->auth_tag);       // out

#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(authenticated, sizeof authenticated, "authenticated");
  dump(auth_key, sizeof auth_key, "auth_key");
  dump(env->auth_tag, crypto_auth_hmacsha512_BYTES, "auth_tag");
#endif
  sodium_munlock(auth_key, sizeof auth_key);

#if (defined CFRG_TEST_VEC || defined TRACE)
  dump((uint8_t *)env, OPAQUE_ENVELOPE_BYTES, "envU");
#endif

  return 0;
}

// (StorePwdFile, sid , U, pw): S computes k_s ←_R Z_q , rw := F_k_s (pw),
// p_s ←_R Z_q , p_u ←_R Z_q , P_s := g^p_s , P_u := g^p_u , c ← AuthEnc_rw (p_u, P_u, P_s);
// it records file[sid] := {k_s, p_s, P_s, P_u, c}.
int opaque_Register(const uint8_t *pwdU, const uint16_t pwdU_len,
                    const uint8_t skS[crypto_scalarmult_SCALARBYTES],
                    const Opaque_Ids *ids,
                    uint8_t _rec[OPAQUE_USER_RECORD_LEN],
                    uint8_t export_key[crypto_hash_sha512_BYTES]) {
  Opaque_UserRecord *rec = (Opaque_UserRecord *)_rec;

#ifdef TRACE
  dump(ids->idU, ids->idU_len,"idU ");
  dump(ids->idS, ids->idS_len,"idS ");
#endif

  // k_s ←_R Z_q
  // 1. (kU, _) = KeyGen()
  oprf_KeyGen(rec->kU);

  // rw := F_k_s (pw),
  uint8_t rwdU[OPAQUE_RWDU_BYTES];
  if(-1==sodium_mlock(rwdU,sizeof rwdU)) {
    return -1;
  }

  if(prf(pwdU, pwdU_len, rec->kU, rwdU)!=0) {
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
#ifdef TRACE
  dump(rwdU, sizeof rwdU, "rwdU");
#endif

  // p_s ←_R Z_q
  if(skS==NULL) {
    randombytes(rec->skS, crypto_scalarmult_SCALARBYTES); // random server secret key
  } else {
    memcpy(rec->skS, skS, crypto_scalarmult_SCALARBYTES);
  }

  // P_s := g^p_s
  uint8_t server_public_key[crypto_scalarmult_BYTES];
  crypto_scalarmult_ristretto255_base(server_public_key, rec->skS);

  uint8_t client_private_key[crypto_scalarmult_SCALARBYTES];
  // todo mlock this
  if(0!=skU_from_rwd(rwdU, (uint8_t*) &rec->recU.envelope, client_private_key)) {
    sodium_munlock(rwdU, sizeof rwdU);
    return -1;
  }
  // P_u := g^p_u
  crypto_scalarmult_base(rec->recU.client_public_key, client_private_key);

  if(0!=create_envelope(rwdU, server_public_key, ids, &rec->recU.envelope, rec->recU.client_public_key, rec->recU.masking_key, export_key)) {
    sodium_munlock(rwdU, sizeof rwdU);
    return -1;
  }

#ifdef TRACE
  dump(_rec, OPAQUE_USER_RECORD_LEN, "user rec");
#endif
  return 0;
}

//(UsrSession, sid , ssid , S, pw): U picks r, x_u ←_R Z_q ; sets α := (H^0(pw))^r and
//X_u := g^x_u ; sends α and X_u to S.
// more or less corresponds to CreateCredentialRequest in the irtf draft
int opaque_CreateCredentialRequest(const uint8_t *pwdU, const uint16_t pwdU_len, uint8_t _sec[OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len], uint8_t _pub[OPAQUE_USER_SESSION_PUBLIC_LEN]) {
  Opaque_UserSession_Secret *sec = (Opaque_UserSession_Secret*) _sec;
  Opaque_UserSession *pub = (Opaque_UserSession*) _pub;
#ifdef TRACE
  memset(_sec, 0, OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len);
  memset(_pub, 0, OPAQUE_USER_SESSION_PUBLIC_LEN);
#endif

  // 1. (blind, M) = Blind(pwdU)
  if(0!=oprf_Blind(pwdU, pwdU_len, sec->blind, pub->M)) return -1;
#ifdef TRACE
  dump(_sec,OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len, "sec ");
  dump(_pub,OPAQUE_USER_SESSION_PUBLIC_LEN, "pub ");
#endif
  memcpy(sec->M, pub->M, crypto_core_ristretto255_BYTES);

  // x_u ←_R Z_q
#ifdef CFRG_TEST_VEC
  const uint8_t client_private_keyshare[32] = {
           // "client_private_keyshare": "4230d62ea740b13e178185fc517cf2c313e6908c4cd9fb42154870ff3490c608"
           0x42, 0x30, 0xd6, 0x2e, 0xa7, 0x40, 0xb1, 0x3e,
           0x17, 0x81, 0x85, 0xfc, 0x51, 0x7c, 0xf2, 0xc3,
           0x13, 0xe6, 0x90, 0x8c, 0x4c, 0xd9, 0xfb, 0x42,
           0x15, 0x48, 0x70, 0xff, 0x34, 0x90, 0xc6, 0x08};
  memcpy(sec->x_u, client_private_keyshare, crypto_scalarmult_SCALARBYTES);
#else
  randombytes(sec->x_u, crypto_scalarmult_SCALARBYTES);
#endif

  // nonceU
#ifdef CFRG_TEST_VEC
  const uint8_t client_nonce[] = {
           // "client_nonce": "804133133e7ee6836c8515752e24bb44d323fef4ead34cde967798f2e9784f69"
           0x80, 0x41, 0x33, 0x13, 0x3e, 0x7e, 0xe6, 0x83,
           0x6c, 0x85, 0x15, 0x75, 0x2e, 0x24, 0xbb, 0x44,
           0xd3, 0x23, 0xfe, 0xf4, 0xea, 0xd3, 0x4c, 0xde,
           0x96, 0x77, 0x98, 0xf2, 0xe9, 0x78, 0x4f, 0x69};
  memcpy(sec->nonceU, client_nonce, OPAQUE_NONCE_BYTES);
#else
  randombytes(sec->nonceU, OPAQUE_NONCE_BYTES);
#endif
  memcpy(pub->nonceU, sec->nonceU, OPAQUE_NONCE_BYTES);

  // X_u := g^x_u
  crypto_scalarmult_ristretto255_base(pub->X_u, sec->x_u);

  sec->pwdU_len = pwdU_len;
  memcpy(sec->pwdU, pwdU, pwdU_len);

#ifdef TRACE
  dump(_sec,OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len, "sec ");
  dump(_pub,OPAQUE_USER_SESSION_PUBLIC_LEN, "pub ");
#endif
  return 0;
}

// more or less corresponds to CreateCredentialResponse in the irtf draft
// 2. (SvrSession, sid , ssid ): On input α from U, S proceeds as follows:
// (a) Checks that α ∈ G^∗ If not, outputs (abort, sid , ssid ) and halts;
// (b) Retrieves file[sid] = {k_s, p_s, P_s, P_u, c};
// (c) Picks x_s ←_R Z_q and computes β := α^k_s and X_s := g^x_s ;
// (d) Computes K := KE(p_s, x_s, P_u, X_u) and SK := f K (0);
// (e) Sends β, X s and c to U;
// (f) Outputs (sid , ssid , SK).
int opaque_CreateCredentialResponse(const uint8_t _pub[OPAQUE_USER_SESSION_PUBLIC_LEN], const uint8_t _rec[OPAQUE_USER_RECORD_LEN], const Opaque_Ids *ids, const uint8_t *ctx, const uint16_t ctx_len, uint8_t _resp[OPAQUE_SERVER_SESSION_LEN], uint8_t sk[OPAQUE_SHARED_SECRETBYTES], uint8_t authU[crypto_auth_hmacsha512_BYTES]) {

  Opaque_UserSession *pub = (Opaque_UserSession *) _pub;
  Opaque_UserRecord *rec = (Opaque_UserRecord *) _rec;
  Opaque_ServerSession *resp = (Opaque_ServerSession *) _resp;

#ifdef TRACE
  dump(_pub, sizeof(Opaque_UserSession), "session srv pub ");
  dump(_rec, OPAQUE_USER_SESSION_PUBLIC_LEN, "session srv rec ");
#endif

  // (a) Checks that α ∈ G^∗ . If not, outputs (abort, sid , ssid ) and halts;
  if(crypto_core_ristretto255_is_valid_point(pub->M)!=1) return -1;

  // (b) Retrieves file[sid] = {k_s, p_s, P_s, P_u, c};
  // provided as parameter rec
#ifdef TRACE
  dump(rec->kU, sizeof(rec->kU), "session srv kU ");
  dump(pub->M, sizeof(pub->M), "session srv M ");
#endif

  // computes β := α^k_s
  // 1. Z = Evaluate(DeserializeScalar(credentialFile.kU), request.data)
  if (oprf_Evaluate(rec->kU, pub->M, resp->Z) != 0) {
    return -1;
  }
#ifdef CFRG_TEST_VEC
  dump(resp->Z, sizeof resp->Z, "EvaluationElement");
#endif

  // 4. masking_nonce = random(Nn)
  // 5. credential_response_pad = Expand(record.masking_key, concat(masking_nonce, "CredentialResponsePad"), Npk + Ne)
#ifdef CFRG_TEST_VEC
  struct {
    uint8_t nonce[32];
    uint8_t dst[21];
  } __attribute((packed)) masking_info = {
      .nonce = {
  // "masking_nonce": "54f9341ca183700f6b6acf28dbfe4a86afad788805de49f2d680ab86ff39ed7f"
           0x54, 0xf9, 0x34, 0x1c, 0xa1, 0x83, 0x70, 0x0f,
           0x6b, 0x6a, 0xcf, 0x28, 0xdb, 0xfe, 0x4a, 0x86,
           0xaf, 0xad, 0x78, 0x88, 0x05, 0xde, 0x49, 0xf2,
           0xd6, 0x80, 0xab, 0x86, 0xff, 0x39, 0xed, 0x7f},
      .dst = "CredentialResponsePad"};
#else
  struct {
    uint8_t nonce[32];
    uint8_t dst[21];
  } __attribute((packed)) masking_info = {
      .nonce = {0},
      .dst = "CredentialResponsePad"};
  randombytes(masking_info.nonce, sizeof masking_info.nonce);
#endif
  uint8_t response_pad[crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)];
  crypto_kdf_hkdf_sha512_expand(response_pad, sizeof response_pad,
                                (const char*) &masking_info, sizeof masking_info,
                                rec->recU.masking_key);
  memcpy(resp->masking_nonce, masking_info.nonce, sizeof masking_info.nonce);

  // recalc server_public_key as we need it for the next step
  uint8_t pkS[crypto_scalarmult_BYTES];
  crypto_scalarmult_ristretto255_base(pkS, rec->skS);
#ifdef CFRG_TEST_VEC
  dump(pkS, sizeof pkS, "server_public_key");
#endif

  memcpy(resp->masked_response, pkS, sizeof pkS);

  // 6. masked_response = xor(credential_response_pad, concat(server_public_key, record.envelope))
  unsigned i;
  for(i=0;i<crypto_scalarmult_BYTES;i++)
    resp->masked_response[i] = response_pad[i] ^ resp->masked_response[i];
  for(;i<crypto_scalarmult_BYTES+sizeof(Opaque_Envelope);i++)
    resp->masked_response[i] = response_pad[i] ^ ((uint8_t*)(&rec->recU.envelope))[i-crypto_scalarmult_BYTES];

#ifdef CFRG_TEST_VEC
  dump(_resp, sizeof (resp->Z) + crypto_scalarmult_BYTES+sizeof(Opaque_Envelope) + sizeof(masking_info.nonce), "resp(z+mn+mr)" );
#endif

  // this is the ake function Response() as per the irtf cfrg draft
  // 1. server_nonce = random(Nn)
  // nonceS
#ifdef CFRG_TEST_VEC
  const uint8_t server_nonce[OPAQUE_NONCE_BYTES] = {
           0xf9, 0xc5, 0xec, 0x75, 0xa8, 0xcd, 0x57, 0x13,
           0x70, 0xad, 0xd2, 0x49, 0xe9, 0x9c, 0xb8, 0xa8,
           0xc4, 0x3f, 0x6e, 0xf0, 0x56, 0x10, 0xac, 0x6e,
           0x35, 0x46, 0x42, 0xbf, 0x4f, 0xed, 0xbf, 0x69};
  memcpy(resp->nonceS, server_nonce, OPAQUE_NONCE_BYTES);
#else
  randombytes(resp->nonceS, OPAQUE_NONCE_BYTES);
#endif

  // 2. server_private_keyshare, server_keyshare = GenerateAuthKeyPair()
  // (c) Picks x_s ←_R Z_q
  uint8_t x_s[crypto_scalarmult_SCALARBYTES];
  if(-1==sodium_mlock(x_s,sizeof x_s)) return -1;
#ifdef CFRG_TEST_VEC
  const uint8_t server_private_keyshare[32] = {
           0xf8, 0xe3, 0xe3, 0x15, 0x43, 0xdd, 0x6f, 0xc8,
           0x68, 0x33, 0x29, 0x67, 0x26, 0x77, 0x3d, 0x51,
           0x15, 0x82, 0x91, 0xab, 0x9a, 0xfd, 0x66, 0x6b,
           0xb5, 0x5d, 0xce, 0x83, 0x47, 0x4c, 0x11, 0x01};
  memcpy(x_s, server_private_keyshare, sizeof x_s);
#else
  randombytes(x_s, crypto_scalarmult_SCALARBYTES);
#endif

#ifdef TRACE
  dump(x_s, sizeof(x_s), "session srv x_s ");
#endif
  // X_s := g^x_s;
  crypto_scalarmult_ristretto255_base(resp->X_s, x_s);

#ifdef CFRG_TEST_VEC
  dump(resp->X_s, sizeof(resp->X_s), "server_keyshare");
#endif
#ifdef TRACE
  dump(resp->X_s, sizeof(resp->X_s), "session srv X_s ");
#endif
  // 3. Create inner_ke2 ike2 with (credential_response, server_nonce, server_keyshare)
  // should already be all in place

  // 4. preamble = Preamble(client_identity, ke1, server_identity, ike2)
  // mixing in things from the irtf cfrg spec
  char preamble[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state preamble_state;
  calc_preamble(preamble, &preamble_state, rec->recU.client_public_key, pkS, _pub, resp, ctx, ctx_len, (Opaque_Ids*) ids);
  Opaque_Keys keys;
  sodium_mlock(&keys,sizeof(keys));

  // (d) Computes K := KE(p_s, x_s, P_u, X_u) and SK := f_K(0);
#ifdef TRACE
  dump(rec->skS,crypto_scalarmult_SCALARBYTES, "rec->skS ");
  dump(x_s,crypto_scalarmult_SCALARBYTES, "x_s ");
  //dump(rec->pkU,crypto_scalarmult_BYTES, "rec->pkU ");
  dump(pub->X_u,crypto_scalarmult_BYTES, "pub->X_u ");
#endif
  // 5. ikm = TripleDHIKM(server_secret, ke1.client_keyshare,
  //                server_private_key, ke1.client_keyshare,
  //                server_secret, client_public_key)
  // 6. Km2, Km3, session_key = DeriveKeys(ikm, preamble)
  if(0!=server_3dh(&keys, rec->skS, x_s, rec->recU.client_public_key, pub->X_u, preamble)) {
    sodium_munlock(x_s, sizeof(x_s));
    sodium_munlock(&keys,sizeof(keys));
    return -1;
  }
  sodium_munlock(x_s, sizeof(x_s));
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(keys.sk, sizeof(keys.sk), "srv sk ");
  dump(keys.km2,OPAQUE_HMAC_SHA512_KEYSIZE,"session srv km2 ");
  dump(keys.km3,OPAQUE_HMAC_SHA512_KEYSIZE,"session srv km3 ");
#endif

  // 7. server_mac = MAC(Km2, Hash(preamble))
  opaque_hmacsha512(keys.km2,
                    (uint8_t*)preamble,                  // in
                    crypto_hash_sha512_BYTES,            // len(in)
                    resp->auth);                         // out
#ifdef TRACE
  dump(resp->auth, sizeof resp->auth, "resp->auth ");
  dump(keys.km2, sizeof keys.km2, "km2 ");
#endif

  // 8. expected_client_mac = MAC(Km3, Hash(concat(preamble, server_mac))
  crypto_hash_sha512_update(&preamble_state, resp->auth, crypto_auth_hmacsha512_BYTES);
  crypto_hash_sha512_final(&preamble_state, (uint8_t *) preamble);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(resp->auth, crypto_auth_hmacsha512_BYTES, "server mac");
  dump((uint8_t*)preamble, sizeof preamble, "auth preamble");
#endif
  crypto_auth_hmacsha512(authU,                               // out
                         (uint8_t*)preamble,                  // in
                         crypto_hash_sha512_BYTES,            // len(in)
                         keys.km3);                           // key

  memcpy(sk,keys.sk,sizeof(keys.sk));
  sodium_munlock(&keys,sizeof(keys));

#ifdef TRACE
  dump(resp->auth, sizeof(resp->auth), "session srv auth ");
  dump(authU, crypto_auth_hmacsha512_BYTES, "authU");
#endif

  return 0;
}

// more or less corresponds to RecoverCredentials in the irtf draft
// 3. On β, X_s and c from S, U proceeds as follows:
// (a) Checks that β ∈ G ∗ . If not, outputs (abort, sid , ssid ) and halts;
// (b) Computes rw := H(key, pw|β^1/r );
// (c) Computes AuthDec_rw(c). If the result is ⊥, outputs (abort, sid , ssid ) and halts.
//     Otherwise sets (p_u, P_u, P_s ) := AuthDec_rw (c);
// (d) Computes K := KE(p_u, x_u, P_s, X_s) and SK := f_K(0);
// (e) Outputs (sid, ssid, SK).
int opaque_RecoverCredentials(const uint8_t _resp[OPAQUE_SERVER_SESSION_LEN],
                              const uint8_t *_sec/*[OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len]*/,
                              const uint8_t *ctx, const uint16_t ctx_len,
                              const Opaque_Ids *ids0,
                              const uint8_t _pub[OPAQUE_USER_SESSION_PUBLIC_LEN],
                              uint8_t sk[OPAQUE_SHARED_SECRETBYTES],
                              uint8_t authU[crypto_auth_hmacsha512_BYTES],
                              uint8_t export_key[crypto_hash_sha512_BYTES]) {

  Opaque_ServerSession *resp = (Opaque_ServerSession *) _resp;
  Opaque_UserSession_Secret *sec = (Opaque_UserSession_Secret *) _sec;

#ifdef TRACE
  dump(sec->pwdU,sec->pwdU_len, "session user finish pwdU ");
  dump(_sec,OPAQUE_USER_SESSION_SECRET_LEN, "session user finish sec ");
  dump(_resp,OPAQUE_SERVER_SESSION_LEN, "session user finish resp ");
#endif

  // 1. (client_private_key, server_public_key, export_key) =
  //  RecoverCredentials(state.password, state.blind, ke2.CredentialResponse,
  //                     server_identity, client_identity)
  // 1.1. y = Finalize(password, blind, response.data, nil)
  // 1.2. randomized_pwd = Extract("", concat(y, Harden(y, params)))
  uint8_t N[crypto_core_ristretto255_BYTES];
  if(-1==sodium_mlock(N, sizeof N)) return -1;
  // 1. N = Unblind(blind, response.data)
  if(0!=oprf_Unblind(sec->blind, resp->Z, N)) {
    sodium_munlock(N, sizeof N);
    return -1;
  }
#ifdef CFRG_TEST_VEC
  dump(N, sizeof N, "unblinded");
#endif

  // rw = H(pw, β^(1/r))
  uint8_t rwdU[OPAQUE_RWDU_BYTES];
  if(-1==sodium_mlock(rwdU,sizeof rwdU)) {
    sodium_munlock(N, sizeof N);
    return -1;
  }
  // 1.2. y = Finalize(pwdU, N, "OPAQUE01")
  if(0!=oprf_Finalize(sec->pwdU, sec->pwdU_len, N, OPAQUE_FINALIZE_INFO, OPAQUE_FINALIZE_INFO_LEN, rwdU)) {
    sodium_munlock(N, sizeof N);
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  sodium_munlock(N,sizeof N);

#ifdef CFRG_TEST_VEC
  dump(rwdU, sizeof rwdU, "rwdU");
#endif

  // 1.3. masking_key = HKDF-Expand(randomized_pwd, "MaskingKey", Nh)
  const uint8_t masking_key_info[10]="MaskingKey";
  uint8_t masking_key[crypto_hash_sha512_BYTES];
  crypto_kdf_hkdf_sha512_expand(masking_key, crypto_hash_sha512_BYTES,
                                (const char*) masking_key_info, sizeof masking_key_info,
                                rwdU);

  // 1.4. credential_response_pad = Expand(masking_key,
  //        concat(response.masking_nonce, "CredentialResponsePad"), Npk + Ne)

  // 1.5. credential_response_pad = Expand(record.masking_key, concat(masking_nonce, "CredentialResponsePad"), Npk + Ne)
  struct {
    uint8_t nonce[32];
    uint8_t dst[21];
  } __attribute((packed)) masking_info = {
      .nonce = {0},
      .dst = "CredentialResponsePad"};
  memcpy(masking_info.nonce, resp->masking_nonce, sizeof masking_info.nonce);

  uint8_t response_pad[crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)];
  crypto_kdf_hkdf_sha512_expand(response_pad, sizeof response_pad,
                                (const char*) &masking_info, sizeof masking_info,
                                masking_key);

  // 1.5. concat(server_public_key, envelope) = xor(credential_response_pad,
  //                                            response.masked_response)
  Opaque_Envelope env;
  uint8_t server_public_key[crypto_scalarmult_BYTES], *env_ptr=(uint8_t*) &env;
  unsigned i;
  for(i=0;i<crypto_scalarmult_BYTES;i++)
    server_public_key[i] = response_pad[i] ^ resp->masked_response[i];
  for(;i<crypto_scalarmult_BYTES+sizeof(Opaque_Envelope);i++)
    env_ptr[i-crypto_scalarmult_BYTES] = response_pad[i] ^ resp->masked_response[i];

#ifdef CFRG_TEST_VEC
  dump(server_public_key, sizeof server_public_key, "server_public_key");
  dump(env.nonce, sizeof env.nonce, "env.nonce");
  dump(env.auth_tag, sizeof env.auth_tag, "env.auth_tag");
#endif

  // 1.6. (client_private_key, export_key) =
  //  Recover(randomized_pwd, server_public_key, envelope,
  //                  server_identity, client_identity)

  uint8_t concated[OPAQUE_ENVELOPE_NONCEBYTES+10],
    *label = concated+OPAQUE_ENVELOPE_NONCEBYTES;
  memcpy(concated, env.nonce, OPAQUE_ENVELOPE_NONCEBYTES);

  // 1.6.1. auth_key = Expand(randomized_pwd, concat(envelope.nonce, "AuthKey"), Nh)
  uint8_t auth_key[OPAQUE_HMAC_SHA512_KEYSIZE];
  if(-1==sodium_mlock(auth_key, sizeof auth_key)) {
    return -1;
  }
  memcpy(label, "AuthKey", 7);
  crypto_kdf_hkdf_sha512_expand(auth_key, sizeof auth_key,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+7,
                                rwdU);

#ifdef TRACE
  dump(auth_key,sizeof auth_key, "auth_key ");
#endif

  // 1.6.2. export_key = Expand(randomized_pwd, concat(envelope.nonce, "ExportKey", Nh)
  memcpy(label, "ExportKey", 9);
#ifdef CFRG_TEST_VEC
  dump(concated, OPAQUE_ENVELOPE_NONCEBYTES+9, "export_key_info");
#endif
  crypto_kdf_hkdf_sha512_expand(export_key, crypto_hash_sha512_BYTES,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+9,
                                rwdU);
#ifdef TRACE
  dump(export_key,crypto_hash_sha512_BYTES, "export_key ");
#endif

  // 1.6.3. seed = Expand(randomized_pwd, concat(envelope.nonce, "PrivateKey"), Nseed)
  memcpy(label, "PrivateKey", 10);
  uint8_t seed[crypto_core_ristretto255_SCALARBYTES];
  crypto_kdf_hkdf_sha512_expand(seed, crypto_core_ristretto255_SCALARBYTES,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+10,
                                rwdU);

  // 1.6.4. client_private_key, client_public_key = DeriveAuthKeyPair(seed)
  uint8_t client_secret_key[crypto_scalarmult_SCALARBYTES];
  if(-1==sodium_mlock(client_secret_key, sizeof client_secret_key)) {
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  uint8_t dst[24]="OPAQUE-DeriveAuthKeyPair";
  if(0!=voprf_hash_to_scalar(seed, sizeof seed, dst, sizeof dst, client_secret_key)) {
    sodium_munlock(client_secret_key, sizeof client_secret_key);
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  // P_u := g^p_u
  uint8_t client_public_key[crypto_scalarmult_BYTES];
  crypto_scalarmult_ristretto255_base(client_public_key, client_secret_key);
#ifdef CFRG_TEST_VEC
  dump(client_secret_key, crypto_scalarmult_SCALARBYTES, "client_secret_key");
  dump(client_public_key, crypto_scalarmult_BYTES, "client_public_key");
#endif

  // 1.6.5. cleartext_creds = CreateCleartextCredentials(server_public_key,
  //                  client_public_key, server_identity, client_identity)

  Opaque_Ids ids;
  fix_ids(client_public_key, server_public_key, ids0, &ids);
  uint8_t authenticated[OPAQUE_NONCE_BYTES+
                        crypto_scalarmult_BYTES+
                        ids.idS_len+2+
                        ids.idU_len+2],
         *ptr=authenticated;

  // nonce
  memcpy(ptr, env.nonce, OPAQUE_NONCE_BYTES);
  ptr+=OPAQUE_NONCE_BYTES;
  // server_public_key
  memcpy(ptr, server_public_key, crypto_scalarmult_BYTES);
  ptr+=crypto_scalarmult_BYTES;
  // server_identity
  uint16_t size = htons(ids.idS_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids.idS,ids.idS_len);
  ptr+=ids.idS_len;
  // client_identity
  size = htons(ids.idU_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids.idU,ids.idU_len);

  // 1.6.6. expected_tag = MAC(auth_key, concat(envelope.nonce, cleartext_creds))
  uint8_t auth_tag[crypto_auth_hmacsha512_BYTES];
  opaque_hmacsha512(auth_key,             // key
                    authenticated,        // in
                    sizeof authenticated, // len(in)
                    auth_tag);            // out

#ifdef CFRG_TEST_VEC
  dump(authenticated, sizeof authenticated, "authenticated");
  dump(auth_key, sizeof auth_key, "auth_key");
  dump(env.auth_tag, crypto_auth_hmacsha512_BYTES, "env auth_tag");
  dump(auth_tag, crypto_hash_sha512_BYTES, "auth tag ");
#endif
  sodium_munlock(auth_key, sizeof auth_key);

#ifdef TRACE
  dump(auth_tag, crypto_hash_sha512_BYTES, "auth tag ");
#endif

  // 1.6.7. If !ct_equal(envelope.auth_tag, expected_tag),
  //   raise KeyRecoveryError
  if(0!=sodium_memcmp(env.auth_tag, auth_tag, sizeof auth_tag)) {
    // todo clear all
    return -1;
  }

  // 2. (ke3, session_key) =
  //  ClientFinalize(client_identity, client_private_key, server_identity,

  // 2.2. preamble = Preamble(client_identity, state.ke1, server_identity, ke2.inner_ke2)
  char preamble[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state preamble_state;
  calc_preamble(preamble, &preamble_state, client_public_key, server_public_key, _pub, resp, ctx, ctx_len, &ids);

  Opaque_Keys keys;
  sodium_mlock(&keys,sizeof(keys));

  // 2.1. ikm = TripleDHIKM(state.client_secret, ke2.server_keyshare,
  //  state.client_secret, server_public_key, client_private_key, ke2.server_keyshare)
  // 2.3. Km2, Km3, session_key = DeriveKeys(ikm, preamble)
  if(0!=user_3dh(&keys, client_secret_key, sec->x_u, server_public_key, resp->X_s, preamble)) {
    sodium_munlock(&keys, sizeof(keys));
    return -1;
  }

  // 2.4. expected_server_mac = MAC(Km2, Hash(preamble))
  uint8_t authS[crypto_auth_hmacsha512_BYTES];
  opaque_hmacsha512(keys.km2,
                    (uint8_t*)preamble,                  // in
                    crypto_hash_sha512_BYTES,            // len(in)
                    authS);                              // out

  // 2.5. If !ct_equal(ke2.server_mac, expected_server_mac),
  //   raise HandshakeError
  if (sodium_memcmp(authS, resp->auth, sizeof authS)!=0) {
    // todo clear all
    return -1;
  }

  // 2.6. client_mac = MAC(Km3, Hash(concat(preamble, expected_server_mac))
  crypto_hash_sha512_update(&preamble_state, authS, crypto_auth_hmacsha512_BYTES);
  crypto_hash_sha512_final(&preamble_state, (uint8_t *) preamble);
  crypto_auth_hmacsha512(authU,                               // out
                         (uint8_t*)preamble,                  // in
                         crypto_hash_sha512_BYTES,            // len(in)
                         keys.km3);                           // key

  // 2.7. Create KE3 ke3 with client_mac
  // 2.8. Output (ke3, session_key)
  memcpy(sk,keys.sk,sizeof(keys.sk));

  sodium_munlock(&keys, sizeof(keys));
  return 0;
}

// extra function to implement the hmac based auth as defined in the irtf cfrg draft
int opaque_UserAuth(const uint8_t authU0[crypto_auth_hmacsha512_BYTES], const uint8_t authU[crypto_auth_hmacsha512_BYTES]) {
    return sodium_memcmp(authU0, authU, crypto_auth_hmacsha512_BYTES);
}

// variant where the secrets of U never touch S unencrypted

// U computes: blinded PW
// called CreateRegistrationRequest in the irtf cfrg rfc draft
int opaque_CreateRegistrationRequest(const uint8_t *pwdU, const uint16_t pwdU_len, uint8_t _sec[OPAQUE_REGISTER_USER_SEC_LEN+pwdU_len], uint8_t M[crypto_core_ristretto255_BYTES]) {
  Opaque_RegisterUserSec *sec = (Opaque_RegisterUserSec *) _sec;
  memcpy(&sec->pwdU, pwdU, pwdU_len);
  sec->pwdU_len = pwdU_len;
  // 1. (blind, M) = Blind(pwdU)
  return oprf_Blind(pwdU, pwdU_len, sec->blind, M);
}

// initUser: S
// (1) checks α ∈ G^∗ If not, outputs (abort, sid , ssid ) and halts;
// (2) generates k_s ←_R Z_q,
// (3) computes: β := α^k_s,
// (4) finally generates: p_s ←_R Z_q, P_s := g^p_s;
// called CreateRegistrationResponse in the irtf cfrg rfc draft
int opaque_CreateRegistrationResponse(const uint8_t M[crypto_core_ristretto255_BYTES], const uint8_t skS[crypto_scalarmult_SCALARBYTES], uint8_t _sec[OPAQUE_REGISTER_SECRET_LEN], uint8_t _pub[OPAQUE_REGISTER_PUBLIC_LEN]) {
  Opaque_RegisterSrvSec *sec = (Opaque_RegisterSrvSec *) _sec;
  Opaque_RegisterSrvPub *pub = (Opaque_RegisterSrvPub *) _pub;

  // (a) Checks that α ∈ G^∗ . If not, outputs (abort, sid , ssid ) and halts;
  if(crypto_core_ristretto255_is_valid_point(M)!=1) return -1;

  // k_s ←_R Z_q
  // 1. (kU, _) = KeyGen()
  oprf_KeyGen(sec->kU);

  // computes β := α^k_s
  // 2. Z = Evaluate(kU, request.data)
  if (oprf_Evaluate(sec->kU, M, pub->Z) != 0) {
    return -1;
  }
#ifdef CFRG_TEST_VEC
  dump(pub->Z, sizeof pub->Z, "EvaluationElement");
#endif
#ifdef TRACE
  dump((uint8_t*) pub->Z, sizeof pub->Z, "Z ");
#endif

  if(skS==NULL) {
    randombytes(sec->skS, crypto_scalarmult_SCALARBYTES); // random server long-term key
  } else {
    memcpy(sec->skS, skS, crypto_scalarmult_SCALARBYTES);
  }

#ifdef TRACE
  dump((uint8_t*) sec->skS, sizeof sec->skS, "skS ");
#endif
  // P_s := g^p_s
  crypto_scalarmult_ristretto255_base(pub->pkS, sec->skS);

#ifdef TRACE
  dump((uint8_t*) pub->pkS, sizeof pub->pkS, "pkS ");
#endif

  return 0;
}

// user computes:
// (a) Checks that β ∈ G ∗ . If not, outputs (abort, sid , ssid ) and halts;
// (b) Computes rw := H(key, pw | β^1/r );
// (c) p_u ←_R Z_q
// (d) P_u := g^p_u,
// (e) c ← AuthEnc_rw (p_u, P_u, P_s);
// called FinalizeRequest in the irtf cfrg rfc draft
int opaque_FinalizeRequest(const uint8_t *_sec/*[OPAQUE_REGISTER_USER_SEC_LEN+pwdU_len]*/,
                           const uint8_t _pub[OPAQUE_REGISTER_PUBLIC_LEN],
                           const Opaque_Ids *ids,
                           uint8_t _rec[OPAQUE_REGISTRATION_RECORD_LEN],
                           uint8_t export_key[crypto_hash_sha512_BYTES]) {

  Opaque_RegisterUserSec *sec = (Opaque_RegisterUserSec *) _sec;
  Opaque_RegisterSrvPub *pub = (Opaque_RegisterSrvPub *) _pub;
  Opaque_RegistrationRecord *rec = (Opaque_RegistrationRecord *) _rec;

  uint8_t N[crypto_core_ristretto255_BYTES];
  if(-1==sodium_mlock(N, sizeof N)) return -1;
  // 1. N = Unblind(blind, response.data)
  if(0!=oprf_Unblind(sec->blind, pub->Z, N)) {
    sodium_munlock(N, sizeof N);
    return -1;
  }
#ifdef CFRG_TEST_VEC
  dump(N, sizeof N, "unblinded");
#endif

  uint8_t rwdU[OPAQUE_RWDU_BYTES];
  if(-1==sodium_mlock(rwdU, sizeof rwdU)) {
    sodium_munlock(N, sizeof N);
    return -1;
  }
  // 2. y = Finalize(pwdU, N, "OPAQUE01")
  if(0!=oprf_Finalize(sec->pwdU, sec->pwdU_len, N, OPAQUE_FINALIZE_INFO, OPAQUE_FINALIZE_INFO_LEN, rwdU)) {
    sodium_munlock(N, sizeof N);
    sodium_munlock(rwdU, sizeof(rwdU));
    return -1;
  }
  sodium_munlock(N,sizeof N);

  if(0!=create_envelope(rwdU, pub->pkS, ids, &rec->envelope, rec->client_public_key, rec->masking_key, export_key)) {
    sodium_munlock(rwdU, sizeof rwdU);
    return -1;
  }
  sodium_munlock(rwdU, sizeof rwdU);

#ifdef CFRG_TEST_VEC
  dump(_rec, OPAQUE_REGISTRATION_RECORD_LEN, "record");
#endif

#ifdef TRACE
  dump(_rec, OPAQUE_REGISTRATION_RECORD_LEN, "registration rec ");
#endif

  return 0;
}

// S records file[sid ] := {k_s, p_s, P_s, P_u, c}.
// called StoreUserRecord in the irtf cfrg rfc draft
void opaque_StoreUserRecord(const uint8_t _sec[OPAQUE_REGISTER_SECRET_LEN], const uint8_t recU[OPAQUE_REGISTRATION_RECORD_LEN], uint8_t _rec[OPAQUE_USER_RECORD_LEN]) {
  Opaque_RegisterSrvSec *sec = (Opaque_RegisterSrvSec *) _sec;
  Opaque_UserRecord *rec = (Opaque_UserRecord *) _rec;

  memcpy(rec->kU, sec->kU, sizeof rec->kU);
  memcpy(rec->skS, sec->skS, crypto_scalarmult_SCALARBYTES);
  memcpy((uint8_t*)&rec->recU, recU, OPAQUE_REGISTRATION_RECORD_LEN);
  //crypto_scalarmult_base(rec->pkS, skS);
#ifdef TRACE
  dump((uint8_t*) rec, OPAQUE_USER_RECORD_LEN, "user rec ");
#endif
}
