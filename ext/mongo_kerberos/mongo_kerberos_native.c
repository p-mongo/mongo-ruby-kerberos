// Copyright (C) 2014-2020 MongoDB, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ruby.h>
#include <sasl/sasl.h>
#include <sasl/saslutil.h>

static VALUE gssapi_error_cls() {
  VALUE mongo = rb_const_get(rb_cObject, rb_intern("Mongo"));
  VALUE gssapi = rb_const_get(mongo, rb_intern("GssapiNative"));
  return rb_const_get(gssapi, rb_intern("Error"));
}

static void raise_gssapi_error(const char *msg, int result) {
  rb_raise(gssapi_error_cls(), "%s (code %d: %s)", msg, result, sasl_errstring(result, NULL, NULL));
}

static void mongo_sasl_conn_free(void* data) {
  sasl_conn_t *conn = (sasl_conn_t*) data;
  if (conn) {
    sasl_dispose(&conn);
    /* We do not set connection to NULL in the Ruby object. */
    /* This is probably fine because this method is supposed to be called */
    /* when the Ruby object is being garbage collected. */
    /* Plus, we don't have the Ruby object reference here to do anything */
    /* with it. */
  }
}

static sasl_conn_t* mongo_sasl_context(VALUE self) {
  sasl_conn_t* conn;
  VALUE context;
  context = rb_iv_get(self, "@context");
  Data_Get_Struct(context, sasl_conn_t, conn);
  printf("returning %p\n", conn);
  return conn;
}

static VALUE a_init(VALUE self, VALUE user_name, VALUE host_name, VALUE service_name, VALUE canonicalize_host_name)
{
  rb_iv_set(self, "@valid", Qtrue);
  rb_iv_set(self, "@user_name", user_name);
  rb_iv_set(self, "@host_name", host_name);
  rb_iv_set(self, "@service_name", service_name);
  rb_iv_set(self, "@canonicalize_host_name", canonicalize_host_name);

  return self;
}

static VALUE valid(VALUE self) {
  return rb_iv_get(self, "@valid");
}

int is_sasl_failure(int result)
{
  if (result < 0) {
    return 1;
  }

  return 0;
}

static int sasl_interact(VALUE self, int id, const char **result, unsigned *len) {
  puts("interact");
  switch (id) {
    case SASL_CB_AUTHNAME:
    case SASL_CB_USER:
    {
      VALUE user_name;
      user_name = rb_iv_get(self, "@user_name");
      *result = RSTRING_PTR(user_name);
      if (len) {
        *len = (int)RSTRING_LEN(user_name);
      }
      return SASL_OK;
    }
  }

  return SASL_FAIL;
}

static VALUE initialize_challenge(VALUE self) {
  int result;
  char encoded_payload[4096];
  const char *raw_payload;
  unsigned int raw_payload_len, encoded_payload_len;
  const char *mechanism_list = "GSSAPI";
  const char *mechanism_selected;
  VALUE context;
  sasl_conn_t *conn;
  sasl_callback_t client_interact [] = {
    { SASL_CB_AUTHNAME, (int (*)(void))sasl_interact, (void*)self },
    { SASL_CB_USER, (int (*)(void))sasl_interact, (void*)self },
    { SASL_CB_LIST_END, NULL, NULL }
  };

  const char *servicename = RSTRING_PTR(rb_iv_get(self, "@service_name"));
  const char *hostname = RSTRING_PTR(rb_iv_get(self, "@host_name"));

  result = sasl_client_new(servicename, hostname, NULL, NULL, client_interact, 0, &conn);

  if (result != SASL_OK) {
    raise_gssapi_error("sasl_client_new failed", result);
  }
  printf("conn=%p\n", conn);

  context = Data_Wrap_Struct(rb_cObject, NULL, mongo_sasl_conn_free, conn);
  /* I'm guessing ruby raises on out of memory condition rather than */
  /* returns NULL, hence no error checking is needed here? */
  
  /* from now on context owns conn */
  /* since mongo_sasl_conn_free cleans up conn, we should NOT call */
  /* sasl_dispose any more in this function. */
  rb_iv_set(self, "@context", context);
  RB_GC_GUARD(context);

  result = sasl_client_start(conn, mechanism_list, NULL, &raw_payload, &raw_payload_len, &mechanism_selected);
  if (is_sasl_failure(result)) {
    raise_gssapi_error("sasl_client_start failed", result);
  }
  if (strcmp(mechanism_selected, "GSSAPI") != 0) {
    rb_raise(gssapi_error_cls(), "sasl_client_start selected an unexpected mechanism: %s", mechanism_selected);
  }

  if (result != SASL_CONTINUE) {
    raise_gssapi_error("sasl_client_start did not return SASL_CONTINUE", result);
  }

  /* cyrus-sasl considers `outmax` (fourth argument) to include the null */
  /* terminator, but this is not documented. Be defensive and exclude it. */
  result = sasl_encode64(raw_payload, raw_payload_len, encoded_payload, sizeof(encoded_payload)-1, &encoded_payload_len);
  if (is_sasl_failure(result)) {
    raise_gssapi_error("sasl_encode64 failed to encode the payload", result);
  }
  if (encoded_payload_len >= sizeof(encoded_payload)) {
    /* Should never happen */
    rb_raise(gssapi_error_cls(), "sasl_encode64 claimed to write %u bytes when up to %lu bytes were allowed", encoded_payload_len, sizeof(encoded_payload));
  }

  encoded_payload[encoded_payload_len] = 0;
  return rb_str_new(encoded_payload, encoded_payload_len);
}

static VALUE evaluate_challenge(VALUE self, VALUE rb_payload) {
  char base_payload[4096], payload[4096];
  const char *step_payload, *out;
  unsigned int step_payload_len, payload_len, base_payload_len, outlen;
  int result;
  sasl_conn_t *conn = mongo_sasl_context(self);
  sasl_interact_t *interact = NULL;

  StringValue(rb_payload);
  step_payload = RSTRING_PTR(rb_payload);
  step_payload_len = (int)RSTRING_LEN(rb_payload);
  puts("eval-1");

  puts(step_payload);
  result = sasl_decode64(step_payload, step_payload_len, base_payload, sizeof(base_payload)-1, &base_payload_len);
  if (is_sasl_failure(result)) {
    raise_gssapi_error("sasl_decode64 failed to decode the payload", result);
  }

  printf("eval-2 %u\n", base_payload_len);
  result = sasl_client_step(conn, base_payload, base_payload_len, &interact, &out, &outlen);
  puts("right");
  if (is_sasl_failure(result)) {
    raise_gssapi_error("sasl_client_step failed", result);
  }

  puts("eval-3");
  result = sasl_encode64(out, outlen, payload, sizeof(payload)-1, &payload_len);
  if (is_sasl_failure(result)) {
    raise_gssapi_error("sasl_encode64 failed to encode the payload", result);
  }

  return rb_str_new(payload, payload_len);
}

VALUE c_GSSAPI_authenticator;

void Init_mongo_kerberos_native() {
  VALUE mongo, auth;
  int result;
  
  result = sasl_client_init(NULL);
  if (result != SASL_OK) {
    rb_raise(rb_eLoadError, "Failed to initialize libsasl2: sasl_client_init failed (code %d: %s)", result, sasl_errstring(result, NULL, NULL));
  }
  
  mongo = rb_const_get(rb_cObject, rb_intern("Mongo"));
  auth = rb_const_get(mongo, rb_intern("Auth"));
  c_GSSAPI_authenticator = rb_define_class_under(auth, "GSSAPIAuthenticator", rb_cObject);
  rb_define_method(c_GSSAPI_authenticator, "initialize", a_init, 4);
  rb_define_method(c_GSSAPI_authenticator, "initialize_challenge", initialize_challenge, 0);
  rb_define_method(c_GSSAPI_authenticator, "evaluate_challenge", evaluate_challenge, 1);
  
  /* Deprecated */
  rb_define_method(rb_cObject, "valid?", valid, 0);
}
