/*
 * ArgonOS port: ESP-IDF - flash partitions.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/flash.h>

#include "esp_partition.h"
#include "esp_mmu_map.h"
#include "sdkconfig.h"

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

uint8_t ag_port_part_subtype(ag_port_part_t p)
{
    return (p != NULL) ? (uint8_t)((const esp_partition_t *)p)->subtype : 0xFFu;
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

/*
 * MAPPING THROUGH esp_mmu_map, NOT spi_flash_mmap (9 Oct 2026).
 *
 * esp_partition_mmap goes through spi_flash_mmap, and with the ROM flash
 * driver (CONFIG_SPI_FLASH_ROM_IMPL) IDF compiles its own spi_flash_mmap out
 * and uses the ROM's (flash_mmap.c: "#if !ESP_ROM_HAS_SPI_FLASH_MMAP ||
 * !CONFIG_SPI_FLASH_ROM_IMPL").  The ROM's keeps a page table of its own that
 * knows nothing of IDF 5's MMU allocator, and on the S3-Zero it answered
 * ESP_ERR_NO_MEM to every request with 300 KB free: no image could run from
 * flash.  esp_mmu_map is what IDF's own spi_flash_mmap calls, so without the
 * ROM driver this is the same path; with it, it is the path that works.
 *
 * The block is page-aligned the way esp_partition_mmap aligns it, and the
 * handle is the page-aligned virtual address esp_mmu_unmap wants back.  A
 * range already mapped (PADDR_SHARED) answers with the existing address and a
 * handle of 0, which unmap leaves alone - IDF's own wrapper does the same.
 */
#define AG_MMU_PAGE ((uint32_t)CONFIG_MMU_PAGE_SIZE)

static ag_err_t map_part(ag_port_part_t p, uint64_t off, size_t len,
                         mmu_mem_caps_t caps, const void **addr,
                         ag_port_map_t *out)
{
    if (p == NULL || addr == NULL || out == NULL || len == 0) {
        return -AG_EINVAL;
    }
    const esp_partition_t *part = (const esp_partition_t *)p;
    if (off > part->size || len > part->size - off) {
        return -AG_ENOSPC;
    }
    const uint32_t paddr = part->address + (uint32_t)off;
    const uint32_t page = paddr & ~(AG_MMU_PAGE - 1u);
    const uint32_t into = paddr - page;

    void           *v = NULL;
    const esp_err_t e = esp_mmu_map(page, len + into, MMU_TARGET_FLASH0, caps,
                                    ESP_MMU_MMAP_FLAG_PADDR_SHARED, &v);
    if (e == ESP_ERR_INVALID_STATE) {
        *addr = (const uint8_t *)v + into;
        *out = 0;
        return AG_OK;
    }
    if (e != ESP_OK) {
        return from_esp(e);
    }
    *addr = (const uint8_t *)v + into;
    *out = (ag_port_map_t)(uintptr_t)v;
    return AG_OK;
}

ag_err_t ag_port_part_map_exec(ag_port_part_t p, uint64_t off, size_t len,
                               const void **addr, ag_port_map_t *out)
{
    return map_part(p, off, len, MMU_MEM_CAP_EXEC | MMU_MEM_CAP_32BIT, addr,
                    out);
}

ag_err_t ag_port_part_map_data(ag_port_part_t p, uint64_t off, size_t len,
                               const void **addr, ag_port_map_t *out)
{
    return map_part(p, off, len, MMU_MEM_CAP_READ | MMU_MEM_CAP_8BIT, addr,
                    out);
}

void ag_port_part_unmap(ag_port_map_t h)
{
    if (h != 0) {
        (void)esp_mmu_unmap((void *)(uintptr_t)h);
    }
}
