/*
    @copyright 2018, pitchfork@ctrlc.hu
    This file is part of pitchforked sphinx.

    pitchforked sphinx is free software: you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    pitchfork is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with pitchforked sphinx. If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <assert.h>
#include "../opaque.h"
#include "../common.h"

static void _dump(const uint8_t *p, const size_t len, const char* msg) {
  int i;
  printf("%s",msg);
  for(i=0;i<len;i++)
    printf("%02x", p[i]);
  printf("\n");
}


int main(void) {
  uint8_t pw[]="simple guessable dictionary password";
  size_t pwlen=strlen((char*) pw);
  uint8_t extra[]="some additional secret data stored in the blob";
  size_t extra_len=strlen((char*) extra);
  uint8_t key[]="some optional key contributed to the opaque protocol";
  size_t key_len=strlen((char*) key);
  unsigned char rec[OPAQUE_USER_RECORD_LEN+extra_len];

  // register user
  printf("storePwdFile\n");
  if(0!=opaque_init_srv(pw, pwlen, extra, extra_len, key, key_len, rec)) return 1;

  // initiate login
  unsigned char sec[OPAQUE_USER_SESSION_SECRET_LEN], pub[OPAQUE_USER_SESSION_PUBLIC_LEN];
  printf("usrSession\n");
  opaque_session_usr_start(pw, pwlen, sec, pub);

  unsigned char resp[OPAQUE_SERVER_SESSION_LEN+extra_len];
  uint8_t sk[32];
  printf("srvSession\n");
  if(0!=opaque_session_srv(pub, rec, resp, sk)) return 1;

  _dump(sk,32,"sk_s: ");

  uint8_t pk[32];
  printf("usrSessionEnd\n");
  uint8_t extra_recovered[extra_len+1], rwd[crypto_secretbox_KEYBYTES];
  extra_recovered[extra_len]=0;
  if(0!=opaque_session_usr_finish(pw, pwlen, resp, sec, key, key_len, pk, extra_recovered, rwd)) return 1;
  printf("recovered extra data: \"%s\"\n", extra_recovered);
  _dump(rwd,32,"rwd: ");
  _dump(pk,32,"sk_u: ");
  assert(sodium_memcmp(sk,pk,sizeof sk)==0);

  // variant where user registration does not leak secrets to server
  uint8_t alpha[crypto_core_ristretto255_BYTES];
  uint8_t r[crypto_core_ristretto255_SCALARBYTES];
  // user initiates:
  printf("newUser\n");
  if(0!=opaque_private_init_usr_start(pw, pwlen, r, alpha)) return 1;
  // server responds
  unsigned char rsec[OPAQUE_REGISTER_SECRET_LEN], rpub[OPAQUE_REGISTER_PUBLIC_LEN];
  printf("initUser\n");
  if(0!=opaque_private_init_srv_respond(alpha, rsec, rpub)) return 1;
  // user commits its secrets
  unsigned char rrec[OPAQUE_USER_RECORD_LEN+extra_len];
  printf("registerUser\n");
  if(0!=opaque_private_init_usr_respond(pw, pwlen, r, rpub, extra, extra_len, key, key_len, rrec, rwd)) return 1;
  // server "saves"
  printf("saveUser\n");
  opaque_private_init_srv_finish(rsec, rpub, rrec);

  printf("userSession\n");
  opaque_session_usr_start(pw, pwlen, sec, pub);
  printf("srvSession\n");
  if(0!=opaque_session_srv(pub, rrec, resp, sk)) return 1;
  _dump(sk,32,"sk_s: ");
  printf("userSessionEnd\n");
  if(0!=opaque_session_usr_finish(pw, pwlen, resp, sec, key, key_len, pk, extra_recovered, rwd)) return 1;
  _dump(pk,32,"sk_u: ");
  _dump(rwd,32,"rwd: ");
  assert(sodium_memcmp(sk,pk,sizeof sk)==0);
  printf("recovered extra data: \"%s\"\n", extra_recovered);

  // authenticate both parties:

  // to authenticate the server to the user, the server sends f_sk(1)
  // to the user, which calculates f_pk(1) and verifies it's the same
  // value as sent by the server.
  uint8_t su[32], us[32];
  sphinx_f(sk, sizeof sk, '1', su);
  sphinx_f(pk, sizeof pk, '1', us);
  _dump(su, 32, "f_sk(1): ");
  _dump(us, 32, "f_pk(1): ");
  assert(0==sodium_memcmp(su,us,32));

  // to authenticate the user to the server, the user sends f_pk(2)
  // to the server, which calculates f_sk(2) and verifies it's the same
  // value as sent by the user.
  sphinx_f(pk, sizeof pk, '2', us);
  sphinx_f(sk, sizeof sk, '2', su);
  _dump(us, 32, "f_pk(2): ");
  _dump(su, 32, "f_sk(2): ");
  assert(0==sodium_memcmp(su,us,32));

  printf("all ok\n");

  return 0;
}
