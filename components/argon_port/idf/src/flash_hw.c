/*
 * ArgonOS port: ESP-IDF - flash partitions.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/flash.h>

#include "esp_partition.h"

static ag_err_t from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                  return AG_OK;
    case ESP_ERR_INVALID_ARG:     return -AG_EINVAL;
    case ESP_ERR_INVALID_SIZE:    return -AG_ENOSPC;
    case ESP_ERR_NOT_FOUND:       return -AG_ENOENT;
    case ESP_ERR_NOT_SUPPORTED:   return -AG_ENOTSUP;
    case ESP_ERR_NO_MEM:          return -AG_ENOMEM;
    case ESP_ERR_INVALID_STATE:   return -AG_EBUSY;
    default:                      return -AG_EIO;
    }
}

ag_port_part_t ag_port_part_find(const char *label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, label);
}

uint64_t ag_port_part_size(ag_port_part_t p)
{
    return (p != NULL) ? (uint64_t)((const esp_partition_t *)p)->size : 0u;
}

uint32_t ag_port_part_erase_size(ag_port_part_t p)
{
    if (p == NULL) {
        return 0u;
    }
    const uint32_t erase = ((const esp_partition_t *)p)->erase_size;
    return (erase > 0u) ? erase : 4096u;
}

ag_err_t ag_port_part_read(ag_port_part_t p, uint64_t off, void *buf,
                           size_t len)
{
    if (p == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    return from_esp(esp_partition_read((const esp_partition_t *)p, (size_t)off,
                                       buf, len));
}

ag_err_t ag_port_part_write(ag_port_part_t p, uint64_t off, const void *buf,
                            size_t len)
{
    if (p == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    return from_esp(esp_partition_write((const esp_partition_t *)p, (size_t)off,
                                        buf, len));
}

ag_err_t ag_port_part_erase(ag_port_part_t p, uint64_t off, size_t len)
{
    if (p == NULL) {
        return -AG_EINVAL;
    }
    return from_esp(esp_partition_erase_range((const esp_partition_t *)p,
                                              (size_t)off, len));
}

ag_err_t ag_port_part_map_exec(ag_port_part_t p, uint64_t off, size_t len,
                               const void **addr, ag_port_map_t *out)
{
    if (p == NULL || addr == NULL || out == NULL) {
        return -AG_EINVAL;
    }

    esp_partition_mmap_handle_t h = 0;
    const esp_err_t             e =
        esp_partition_mmap((const esp_partition_t *)p, (size_t)off, len,
                           ESP_PARTITION_MMAP_INST, addr, &h);
    if (e != ESP_OK) {
        return from_esp(e);
    }
    *out = (ag_port_map_t)h;
    return AG_OK;
}

void ag_port_part_unmap(ag_port_map_t h)
{
    if (h != 0) {
        esp_partition_munmap((esp_partition_mmap_handle_t)h);
    }
}
