//go:build linux && cgo

package credentials

/*
#cgo pkg-config: libsecret-1
#include <libsecret/secret.h>
#include <stdlib.h>
#include <string.h>

static int bottie_keyring(int op, const char *client, const char *redirect,
                         const char *input, size_t input_len, char **output,
                         size_t *output_len, GCancellable *cancel) {
    const SecretSchema schema = {
        "io.github.AyashBera.bottie.Spotify", SECRET_SCHEMA_NONE,
        {{"client-id", SECRET_SCHEMA_ATTRIBUTE_STRING},
         {"redirect-uri", SECRET_SCHEMA_ATTRIBUTE_STRING},
         {"profile", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}}
    };
    int result = 3;
    GError *error = NULL;
    SecretService *service = secret_service_open_sync(SECRET_TYPE_SERVICE, NULL,
        SECRET_SERVICE_OPEN_SESSION, cancel, &error);
    SecretCollection *collection = NULL;
    GHashTable *attributes = NULL;
    GList *items = NULL;
    SecretValue *value = NULL;
    SecretItem *created = NULL;
    if (!service) goto done;
    const char *algorithm = secret_service_get_session_algorithms(service);
    if (!algorithm || strcmp(algorithm, "dh-ietf1024-sha256-aes128-cbc-pkcs7")) goto done;
    collection = secret_collection_for_alias_sync(service, "default",
        SECRET_COLLECTION_NONE, cancel, &error);
    if (!collection) goto done;
    // The session collection is volatile, even if configured as the default.
    if (!strcmp(g_dbus_proxy_get_object_path(G_DBUS_PROXY(collection)),
                "/org/freedesktop/secrets/collection/session")) goto done;
    if (secret_collection_get_locked(collection)) { result = 2; goto done; }
    if (op == 0) { result = 0; goto done; }
    attributes = secret_attributes_build(&schema, "client-id", client,
        "redirect-uri", redirect, "profile", "desktop-v1", NULL);
    items = secret_collection_search_sync(collection, &schema, attributes,
        SECRET_SEARCH_ALL, cancel, &error);
    if (error) goto done;
    if (g_list_length(items) > 1) { result = 4; goto done; }
    if (items && secret_item_get_locked(SECRET_ITEM(items->data))) { result = 2; goto done; }
    if (op == 1) {
        if (!items) { result = 1; goto done; }
        if (!secret_item_load_secret_sync(SECRET_ITEM(items->data), cancel, &error)) goto done;
        value = secret_item_get_secret(SECRET_ITEM(items->data));
        if (!value) goto done;
        gsize size = 0;
        const gchar *data = secret_value_get(value, &size);
        if (size > 12288) { result = 4; goto done; }
        *output = g_memdup2(data, size);
        *output_len = size;
        result = 0;
    } else if (op == 2) {
        value = secret_value_new(input, input_len, "application/json");
        created = secret_item_create_sync(collection, &schema, attributes,
            "Bottie Spotify authorization", value, SECRET_ITEM_CREATE_REPLACE,
            cancel, &error);
        if (created) result = 0;
    } else if (op == 3) {
        if (!items || secret_item_delete_sync(SECRET_ITEM(items->data), cancel, &error)) result = 0;
    }
done:
    if (error && g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) result = 2;
    g_clear_error(&error);
    if (created) g_object_unref(created);
    if (value) secret_value_unref(value);
    g_list_free_full(items, g_object_unref);
    if (attributes) g_hash_table_unref(attributes);
    if (collection) g_object_unref(collection);
    if (service) g_object_unref(service);
    return result;
}
static void bottie_clear(char *data, size_t size) {
    if (data) { volatile char *p = data; while (size--) *p++ = 0; g_free(data); }
}
*/
import "C"

import (
	"context"
	"sync"
	"time"
	"unsafe"
)

func (k *Keyring) call(ctx context.Context, op int, input []byte) ([]byte, error) {
	ctx, stop := context.WithTimeout(ctx, 4*time.Second)
	defer stop()
	if ctx.Err() != nil {
		return nil, ErrUnavailable
	}
	cancel := C.g_cancellable_new()
	var worker sync.WaitGroup
	done := make(chan struct{})
	worker.Go(func() {
		select {
		case <-ctx.Done():
			C.g_cancellable_cancel(cancel)
		case <-done:
		}
	})
	defer func() { close(done); worker.Wait(); C.g_object_unref(C.gpointer(cancel)) }()
	client, redirect := C.CString(k.clientID), C.CString(k.redirectURI)
	defer C.free(unsafe.Pointer(client))
	defer C.free(unsafe.Pointer(redirect))
	data := C.CBytes(input)
	defer C.bottie_clear((*C.char)(data), C.size_t(len(input)))
	var output *C.char
	var size C.size_t
	code := C.bottie_keyring(C.int(op), client, redirect, (*C.char)(data), C.size_t(len(input)), &output, &size, cancel)
	defer C.bottie_clear(output, size)
	switch code {
	case 0:
		return C.GoBytes(unsafe.Pointer(output), C.int(size)), nil
	case 1:
		return nil, ErrNotFound
	case 2:
		return nil, ErrLocked
	case 4:
		return nil, ErrInvalid
	default:
		return nil, ErrUnavailable
	}
}
