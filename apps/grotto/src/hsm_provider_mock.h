/* hsm_provider_mock.h - Mock HSM provider for testing
 *
 * This module provides a mock HSM provider implementation for testing
 * HSM workflows without actual hardware. It simulates device detection,
 * key operations, and signing using software-based cryptography.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_HSM_PROVIDER_MOCK_H
#define APPS_GNOSTR_SIGNER_HSM_PROVIDER_MOCK_H

#include "hsm_provider.h"

G_BEGIN_DECLS

#define GN_TYPE_HSM_PROVIDER_MOCK (gn_hsm_provider_mock_get_type())

G_DECLARE_FINAL_TYPE(GnHsmProviderMock, gn_hsm_provider_mock, GN, HSM_PROVIDER_MOCK, GObject)

/**
 * gn_hsm_provider_mock_new:
 *
 * Creates a new mock HSM provider instance.
 *
 * Returns: (transfer full): A new #GnHsmProviderMock
 */
GnHsmProviderMock *gn_hsm_provider_mock_new(void);

/**
 * gn_hsm_provider_mock_add_device:
 * @self: A #GnHsmProviderMock
 * @slot_id: Slot ID for the device
 * @label: Device label
 * @needs_pin: Whether device requires PIN
 *
 * Adds a simulated device to the mock provider.
 */
void gn_hsm_provider_mock_add_device(GnHsmProviderMock *self,
                                     guint64 slot_id,
                                     const gchar *label,
                                     gboolean needs_pin);

/**
 * gn_hsm_provider_mock_remove_device:
 * @self: A #GnHsmProviderMock
 * @slot_id: Slot ID of device to remove
 *
 * Removes a simulated device from the mock provider.
 */
void gn_hsm_provider_mock_remove_device(GnHsmProviderMock *self,
                                        guint64 slot_id);

/**
 * gn_hsm_provider_mock_set_pin:
 * @self: A #GnHsmProviderMock
 * @slot_id: Slot ID of device
 * @pin: PIN to set
 *
 * Sets the expected PIN for a simulated device.
 */
void gn_hsm_provider_mock_set_pin(GnHsmProviderMock *self,
                                  guint64 slot_id,
                                  const gchar *pin);

/**
 * gn_hsm_provider_mock_simulate_error:
 * @self: A #GnHsmProviderMock
 * @error_code: Error code to return on next operation
 *
 * Configures the mock to return an error on the next operation.
 * Useful for testing error handling paths.
 */
void gn_hsm_provider_mock_simulate_error(GnHsmProviderMock *self,
                                         GnHsmError error_code);

/**
 * gn_hsm_provider_mock_clear_simulated_error:
 * @self: A #GnHsmProviderMock
 *
 * Clears any simulated error.
 */
void gn_hsm_provider_mock_clear_simulated_error(GnHsmProviderMock *self);

/**
 * gn_hsm_provider_mock_get_operation_count:
 * @self: A #GnHsmProviderMock
 *
 * Gets the number of operations performed on this provider.
 * Useful for verifying test expectations.
 *
 * Returns: Operation count
 */
guint gn_hsm_provider_mock_get_operation_count(GnHsmProviderMock *self);

/**
 * gn_hsm_provider_mock_reset_operation_count:
 * @self: A #GnHsmProviderMock
 *
 * Resets the operation counter to zero.
 */
void gn_hsm_provider_mock_reset_operation_count(GnHsmProviderMock *self);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_HSM_PROVIDER_MOCK_H */
