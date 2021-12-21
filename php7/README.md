# libopaque PHP7 bindings

These bindings provide access to libopaque which implements the
[IRTF CFRG RFC draft](https://github.com/cfrg/draft-irtf-cfrg-opaque)
or you can read the [original paper](https://eprint.iacr.org/2018/163).

## Dependencies

These bindings depend on the following:
 - php7-dev
 - libopaque: https://github.com/stef/libopaque/
 - libsodium

## Building and Installing the opaque php7 extension

```
$ phpize
$ ./configure
$ make
$ make install
```

These commands should build our shared extension “opaque.so” and copy
it into appropriate directory of our PHP installation. To load it, we
need to add a line into our custom php.ini

```
$ vi /etc/php.ini
```

Add the following line:

```
extension=opaque.so
```

Check that extension is loaded and works. `php -m` command prints the
list of loaded extensions:

```
$ php -m | grep opaque
opaque
```

## API

There are 3 data structures that are used by libopaque:

### `Ids`

The IDs of the client (idU) and the server (idS) are passed directly
as seperate parameters to functions that need to handle IDs.

### `PkgConfig`
Configuration of the envelope is handled via a simple array:

```
$cfg=[opaque_InSecEnv, opaque_InSecEnv, opaque_InSecEnv, opaque_InSecEnv, opaque_InSecEnv];
```

The items in this array correspond to the envelope fields in this
order: `skU`, `pkU`, `pkS`, `idU`, `idS`.

### `App_Infos`

The IRTF CFRG draft mentions `info` and `einfo` parameters that can
be used to be bound into the session, these are passed as a simple two-element array:

```php
$infos=["some info", "some einfo"];
```

## One-key Server convenience function

For the case that a setup requires externally generated long-term
server keys the php bindings provide a convenience function
`opaque_create_server_keys()` which can be used to generate a server
key-pair. The functions returns `$pkS` and `$skS` in this order.

## 1-step registration

1-step registration is only specified in the original paper. It is not specified by the IRTF
CFRG draft. 1-step registration has the benefit that the supplied password (`pwd`) can be checked
on the server for password rules (e.g., occurrence in common password
lists). It has the drawback that the password is exposed to the server.

```php
$rec, $export_key = opaque_register($pwd, $idU, $idS, $cfg, $skS);
```

## 4-step registration

Registration as specified in the IRTF CFRG draft consists of the
following 4 steps:

### Step 1: The user creates a registration request.

```php
$sec, $req = opaque_create_registration_request(pwd);
```

- `$pwd` is the user's password.

The user should hold on to `$sec` securely until step 3 of the
registration process. `$req` needs to be passed to the server running
step 2.

### Step 2: The server responds to the registration request.

```php
$sec, $resp = opaque_create_registration_response($req);
```

alternatively, for explicitly providing the server long term private key:

```php
$sec, $resp = opaque_create_registration_response($req, $skS);
```

 - `$req` comes from the user running the previous step.
 - `$skS` is an optional explicitly specified server long-term key

The server should hold onto `$sec` securely until step 4 of the registration process.
`$resp` should be passed to the user running step 3.

### Step 3: The user finalizes the registration using the response from the server.

```php
$rec, $export_key = opaque_finalize_request($sec, $resp, $idU, $idS, $cfg);
```


 - `$sec` contains sensitive data and should be disposed securely after usage in this step.
 - `$resp` comes from the server running the previous step.
 - `$idU` is the clients ID,
 - `$idS` is the servers ID,
 - `$cfg` is an array containing the envelope configuration,

 - `$rec` should be passed to the server running step 4.
 - `$export_key` is an extra secret that can be used to encrypt
   additional data that you might want to store on the server next to
   your record.

### Step 4: The server finalizes the user's record.

```php
$rec = opaque_store_user_record($sec, $rec);
```

 - `$rec` comes from the client running the previous step.
 - `$sec` contains sensitive data and should be disposed securely after usage in this step.

 - `$rec` should be stored by the server associated with the ID of the user.

**Important Note**: Confusingly this function is called `StoreUserRecord`, yet it
does not do any storage. How you want to store the record (`$rec`) is up
to the implementor using this API.

## Establishing an opaque session

After a user has registered with a server, the user can initiate the
AKE and thus request its credentials in the following 3(+1)-step protocol:

### Step 1: The user initiates a credential request.

```php
$req, $sec = opaque_create_credential_request($pwd)
```

 - `$pwd` is the user's password.

The user should hold onto `$sec` securely until step 3 of the protocol.
`$pub` needs to be passed to the server running step 2.

### Step 2: The server responds to the credential request.

```php
$resp, $sk, $sec = opaque_create_credential_response($req, $rec, $idU, $idS, $cfg, $infos);
```

 - `$req` comes from the user running the previous step.
 - `$rec` is the user's record stored by the server at the end of the registration protocol.
 - `$idU` is the clients ID,
 - `$idS` is the servers ID,
 - `$cfg` is an array containing the envelope configuration,
 - `infos` is an optional array containing two info strings, check the
   IRTF CFRG specification if you need this (probably not) and how to use this.

This function returns:

 - `$resp` needs to be passed to the user running step 3.
 - `$sk` is a shared secret, the result of the AKE.
 - The server should hold onto `$sec` securely until the optional step
   4 of the protocol, if needed. otherwise this value should be
   discarded securely.

### Step 3: The user recovers its credentials from the server's response.

```php
$sk, $authU, $export_key, $idU, $idS = opaque_recover_credentials($resp, $sec, $cfg, $infos, $pkS, $idU, $idS);
```

 - `$resp` comes from the server running the previous step.
 - `$secU` contains sensitive data and should be disposed securely after usage in this step.
 - `$cfg` is an array containing the envelope configuration,
 - `$infos` is an optional array containing two info strings, check the
   IRTF CFRG specification if you need this (probably not) and how to
   use this.
 - `$pkS` is the server's public key, this must be specified if the
   3rd item in cfg is `NotPackaged` otherwise it must be nil
 - `$idU` is the clients ID, this must be specified even if just an
   empty string if the 4th element of `$cfg` is `NotPackaged`,
   otherwise it must be `nil`.
 - `$idS` is the servers ID,t his must be specified even if just an
   empty string if the 5th element of `$cfg` is `NotPackaged`,
   otherwise it must be `nil`.

This function returns:

 - `$sk` is a shared secret, the result of the AKE.
 - `$authU` is an authentication tag that can be passed in step 4 for explicit user authentication.
 - `$export_key` can be used to decrypt additional data stored by the server.
 - `$ids` is an `Ids` struct containing the IDs of the user and the server.

### Step 4 (Optional): The server authenticates the user.

This step is only needed if there is no encrypted channel setup
towards the server using the shared secret.

```php
opaque_user_auth($sec, $authU);
```

 - `$sec` contains sensitive data and should be disposed securely after usage in this step.
 - `$authU` comes from the user running the previous step.

The function returns a boolean `false` in case the authentication failed, otherwise `true`.

