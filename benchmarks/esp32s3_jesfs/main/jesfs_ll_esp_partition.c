#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"

#include "jesfs.h"
#include "jesfs_int.h"

#define CMD_DEEPPOWERDOWN    0xB9
#define CMD_RELEASEDPD       0xAB
#define CMD_RDID             0x9F
#define CMD_WRITEENABLE      0x06
#define CMD_STATUSREG        0x05
#define CMD_READDATA         0x03
#define CMD_BULKERASE        0xC7
#define CMD_PAGEWRITE        0x02
#define CMD_SECTOR4K_ERASE   0x20

#define JESFS_PARTITION_LABEL "jesfs"
#define JESFS_DENSITY_4MB     0x16
#define JESFS_SIM_ID          ((MACRONIX_MANU_TYP_RX << 8) | JESFS_DENSITY_4MB)

typedef struct {
    const esp_partition_t *partition;
    uint32_t addr;
    uint8_t selected;
    uint8_t state;
    uint8_t status;
    uint8_t powerdown;
} jesfs_ll_t;

static const char *TAG = "jesfs_ll";
static jesfs_ll_t s_ll;

static void log_err(const char *op, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", op, esp_err_to_name(err));
    }
}

void sflash_wait_usec(uint32_t usec)
{
    esp_rom_delay_us(usec);
}

int16_t sflash_spi_init(void)
{
    if (s_ll.partition == NULL) {
        s_ll.partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_ANY,
            JESFS_PARTITION_LABEL);
        if (s_ll.partition == NULL) {
            ESP_LOGE(TAG, "partition '%s' not found", JESFS_PARTITION_LABEL);
            return -100;
        }
        ESP_LOGI(TAG, "using partition '%s' at 0x%lx size 0x%lx",
                 s_ll.partition->label,
                 (unsigned long)s_ll.partition->address,
                 (unsigned long)s_ll.partition->size);
        if (s_ll.partition->size < (1UL << JESFS_DENSITY_4MB)) {
            ESP_LOGE(TAG, "partition is smaller than configured JesFS disk");
            return -100;
        }
    }

    s_ll.addr = 0xffffffffu;
    s_ll.selected = 0;
    s_ll.state = 0;
    s_ll.status = 0;
    s_ll.powerdown = 0;
    return 0;
}

void sflash_spi_close(void)
{
}

void sflash_select(void)
{
    s_ll.selected = 1;
    s_ll.state = 0;
}

void sflash_deselect(void)
{
    s_ll.selected = 0;
}

void sflash_spi_read(uint8_t *buf, uint16_t len)
{
    esp_err_t err;

    if (!s_ll.selected || s_ll.state < 128) {
        memset(buf, 0xff, len);
        return;
    }

    switch (s_ll.state) {
    case 128:
        if (len >= 3) {
            buf[0] = (uint8_t)(MACRONIX_MANU_TYP_RX >> 8);
            buf[1] = (uint8_t)MACRONIX_MANU_TYP_RX;
            buf[2] = JESFS_DENSITY_4MB;
        }
        s_ll.state = 0;
        break;

    case 129:
        if (len >= 1) {
            buf[0] = s_ll.status;
        }
        s_ll.status &= (uint8_t)~1u;
        break;

    case 130:
        err = esp_partition_read(s_ll.partition, s_ll.addr, buf, len);
        log_err("read", err);
        s_ll.addr += len;
        break;

    default:
        memset(buf, 0xff, len);
        break;
    }
}

void sflash_spi_write(const uint8_t *buf, uint16_t len)
{
    esp_err_t err;
    uint32_t max_page;

    if (!s_ll.selected || len == 0) {
        return;
    }

    if (s_ll.state == 0) {
        switch (buf[0]) {
        case CMD_DEEPPOWERDOWN:
            s_ll.powerdown = 1;
            break;
        case CMD_RELEASEDPD:
            s_ll.powerdown = 0;
            break;
        case CMD_RDID:
            s_ll.state = 128;
            break;
        case CMD_WRITEENABLE:
            s_ll.status |= 2;
            break;
        case CMD_STATUSREG:
            s_ll.state = 129;
            break;
        case CMD_READDATA:
            if (len >= 4) {
                s_ll.addr = ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[2] << 8) |
                            (uint32_t)buf[3];
                s_ll.state = 130;
            }
            break;
        case CMD_PAGEWRITE:
            if (len >= 4) {
                s_ll.addr = ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[2] << 8) |
                            (uint32_t)buf[3];
                s_ll.state = 1;
            }
            break;
        case CMD_SECTOR4K_ERASE:
            if (len >= 4) {
                s_ll.addr = ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[2] << 8) |
                            (uint32_t)buf[3];
                err = esp_partition_erase_range(s_ll.partition, s_ll.addr, SF_SECTOR_PH);
                log_err("erase sector", err);
                s_ll.status &= (uint8_t)~2u;
            }
            break;
        case CMD_BULKERASE:
            err = esp_partition_erase_range(s_ll.partition, 0, 1UL << JESFS_DENSITY_4MB);
            log_err("bulk erase", err);
            s_ll.status &= (uint8_t)~2u;
            break;
        default:
            break;
        }
        return;
    }

    if (s_ll.state == 1) {
        max_page = 256u - (s_ll.addr & 255u);
        if (len > max_page) {
            ESP_LOGE(TAG, "page write crosses 256-byte page: addr=0x%lx len=%u",
                     (unsigned long)s_ll.addr, len);
            return;
        }
        err = esp_partition_write(s_ll.partition, s_ll.addr, buf, len);
        log_err("write", err);
        s_ll.addr += len;
        s_ll.status &= (uint8_t)~2u;
    }
}
