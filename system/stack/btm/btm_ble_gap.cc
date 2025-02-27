/******************************************************************************
 *
 *  Copyright 2008-2014 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains functions for BLE GAP.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btm_ble"

#include <base/bind.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>

#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include "bta/include/bta_api.h"
#include "common/time_util.h"
#include "device/include/controller.h"
#include "main/shim/acl_api.h"
#include "main/shim/btm_api.h"
#include "main/shim/shim.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"  // UNUSED_ATTR
#include "stack/acl/acl.h"
#include "stack/btm/btm_ble_int.h"
#include "stack/btm/btm_ble_int_types.h"
#include "stack/btm/btm_dev.h"
#include "stack/btm/btm_int_types.h"
#include "stack/gatt/gatt_int.h"
#include "stack/include/acl_api.h"
#include "stack/include/advertise_data_parser.h"
#include "stack/include/ble_scanner.h"
#include "stack/include/bt_types.h"
#include "stack/include/btm_api_types.h"
#include "stack/include/gap_api.h"
#include "stack/include/hci_error_code.h"
#include "stack/include/inq_hci_link_interface.h"
#include "types/ble_address_with_type.h"
#include "types/raw_address.h"

extern tBTM_CB btm_cb;

extern void btm_inq_remote_name_timer_timeout(void* data);
extern bool btm_ble_init_pseudo_addr(tBTM_SEC_DEV_REC* p_dev_rec,
                                     const RawAddress& new_pseudo_addr);
extern bool btm_identity_addr_to_random_pseudo(RawAddress* bd_addr,
                                               tBLE_ADDR_TYPE* p_addr_type,
                                               bool refresh);
extern void btm_ble_batchscan_init(void);
extern void btm_ble_adv_filter_init(void);
extern void btm_clear_all_pending_le_entry(void);
extern const tBLE_BD_ADDR convert_to_address_with_type(
    const RawAddress& bd_addr, const tBTM_SEC_DEV_REC* p_dev_rec);

#define BTM_EXT_BLE_RMT_NAME_TIMEOUT_MS (30 * 1000)
#define MIN_ADV_LENGTH 2
#define BTM_VSC_CHIP_CAPABILITY_RSP_LEN 9
#define BTM_VSC_CHIP_CAPABILITY_RSP_LEN_L_RELEASE \
  BTM_VSC_CHIP_CAPABILITY_RSP_LEN
#define BTM_VSC_CHIP_CAPABILITY_RSP_LEN_M_RELEASE 15
#define BTM_VSC_CHIP_CAPABILITY_RSP_LEN_S_RELEASE 25

namespace {

class AdvertisingCache {
 public:
  /* Set the data to |data| for device |addr_type, addr| */
  const std::vector<uint8_t>& Set(uint8_t addr_type, const RawAddress& addr,
                                  std::vector<uint8_t> data) {
    auto it = Find(addr_type, addr);
    if (it != items.end()) {
      it->data = std::move(data);
      return it->data;
    }

    if (items.size() > cache_max) {
      items.pop_back();
    }

    items.emplace_front(addr_type, addr, std::move(data));
    return items.front().data;
  }

  bool Exist(uint8_t addr_type, const RawAddress& addr) {
    auto it = Find(addr_type, addr);
    if (it != items.end()) {
      return true;
    }
    return false;
  }

  /* Append |data| for device |addr_type, addr| */
  const std::vector<uint8_t>& Append(uint8_t addr_type, const RawAddress& addr,
                                     std::vector<uint8_t> data) {
    auto it = Find(addr_type, addr);
    if (it != items.end()) {
      it->data.insert(it->data.end(), data.begin(), data.end());
      return it->data;
    }

    if (items.size() > cache_max) {
      items.pop_back();
    }

    items.emplace_front(addr_type, addr, std::move(data));
    return items.front().data;
  }

  /* Clear data for device |addr_type, addr| */
  void Clear(uint8_t addr_type, const RawAddress& addr) {
    auto it = Find(addr_type, addr);
    if (it != items.end()) {
      items.erase(it);
    }
  }

  void ClearAll() { items.clear(); }

 private:
  struct Item {
    uint8_t addr_type;
    RawAddress addr;
    std::vector<uint8_t> data;

    Item(uint8_t addr_type, const RawAddress& addr, std::vector<uint8_t> data)
        : addr_type(addr_type), addr(addr), data(data) {}
  };

  std::list<Item>::iterator Find(uint8_t addr_type, const RawAddress& addr) {
    for (auto it = items.begin(); it != items.end(); it++) {
      if (it->addr_type == addr_type && it->addr == addr) {
        return it;
      }
    }
    return items.end();
  }

  /* we keep maximum 7 devices in the cache */
  const size_t cache_max = 7;
  std::list<Item> items;
};

/* Devices in this cache are waiting for eiter scan response, or chained packets
 * on secondary channel */
AdvertisingCache cache;

}  // namespace

#if (BLE_VND_INCLUDED == TRUE)
static tBTM_BLE_CTRL_FEATURES_CBACK* p_ctrl_le_feature_rd_cmpl_cback = NULL;
#endif
/**********PAST & PS *******************/
using StartSyncCb = base::Callback<void(
    uint8_t /*status*/, uint16_t /*sync_handle*/, uint8_t /*advertising_sid*/,
    uint8_t /*address_type*/, RawAddress /*address*/, uint8_t /*phy*/,
    uint16_t /*interval*/)>;
using SyncReportCb = base::Callback<void(
    uint16_t /*sync_handle*/, int8_t /*tx_power*/, int8_t /*rssi*/,
    uint8_t /*status*/, std::vector<uint8_t> /*data*/)>;
using SyncLostCb = base::Callback<void(uint16_t /*sync_handle*/)>;
using SyncTransferCb = base::Callback<void(uint8_t /*status*/, RawAddress)>;
#define MAX_SYNC_TRANSACTION 16
#define SYNC_TIMEOUT (30 * 1000)
#define ADV_SYNC_ESTB_EVT_LEN 16
#define SYNC_LOST_EVT_LEN 3
typedef enum {
  PERIODIC_SYNC_IDLE = 0,
  PERIODIC_SYNC_PENDING,
  PERIODIC_SYNC_ESTABLISHED,
  PERIODIC_SYNC_LOST,
} tBTM_BLE_PERIODIC_SYNC_STATE;

struct alarm_t* sync_timeout_alarm;
typedef struct {
  uint8_t sid;
  RawAddress remote_bda;
  tBTM_BLE_PERIODIC_SYNC_STATE sync_state;
  uint16_t sync_handle;
  bool in_use;
  StartSyncCb sync_start_cb;
  SyncReportCb sync_report_cb;
  SyncLostCb sync_lost_cb;
} tBTM_BLE_PERIODIC_SYNC;

typedef struct {
  bool in_use;
  int conn_handle;
  RawAddress addr;
  SyncTransferCb cb;
} tBTM_BLE_PERIODIC_SYNC_TRANSFER;

static list_t* sync_queue;
static std::mutex sync_queue_mutex_;

typedef struct {
  bool busy;
  uint8_t sid;
  RawAddress address;
  uint16_t skip;
  uint16_t timeout;
} sync_node_t;
typedef struct {
  uint8_t sid;
  RawAddress address;
} remove_sync_node_t;
typedef enum {
  BTM_QUEUE_SYNC_REQ_EVT,
  BTM_QUEUE_SYNC_ADVANCE_EVT,
  BTM_QUEUE_SYNC_CLEANUP_EVT
} btif_queue_event_t;

typedef struct {
  tBTM_BLE_PERIODIC_SYNC p_sync[MAX_SYNC_TRANSACTION];
  tBTM_BLE_PERIODIC_SYNC_TRANSFER sync_transfer[MAX_SYNC_TRANSACTION];
} tBTM_BLE_PA_SYNC_TX_CB;
tBTM_BLE_PA_SYNC_TX_CB btm_ble_pa_sync_cb;
StartSyncCb sync_rcvd_cb;
static bool syncRcvdCbRegistered = false;
static int btm_ble_get_psync_index(uint8_t adv_sid, RawAddress addr);
static void btm_ble_start_sync_timeout(void* data);

/*****************************/
/*******************************************************************************
 *  Local functions
 ******************************************************************************/
static void btm_ble_update_adv_flag(uint8_t flag);
void btm_ble_process_adv_pkt_cont(uint16_t evt_type, tBLE_ADDR_TYPE addr_type,
                                  const RawAddress& bda, uint8_t primary_phy,
                                  uint8_t secondary_phy,
                                  uint8_t advertising_sid, int8_t tx_power,
                                  int8_t rssi, uint16_t periodic_adv_int,
                                  uint8_t data_len, const uint8_t* data,
                                  const RawAddress& original_bda);
static uint8_t btm_set_conn_mode_adv_init_addr(RawAddress& p_peer_addr_ptr,
                                               tBLE_ADDR_TYPE* p_peer_addr_type,
                                               tBLE_ADDR_TYPE* p_own_addr_type);
static void btm_ble_stop_observe(void);
static void btm_ble_fast_adv_timer_timeout(void* data);
static void btm_ble_start_slow_adv(void);
static void btm_ble_inquiry_timer_gap_limited_discovery_timeout(void* data);
static void btm_ble_inquiry_timer_timeout(void* data);
static void btm_ble_observer_timer_timeout(void* data);

enum : uint8_t {
  BTM_BLE_NOT_SCANNING = 0x00,
  BTM_BLE_INQ_RESULT = 0x01,
  BTM_BLE_OBS_RESULT = 0x02,
};

static bool ble_evt_type_is_connectable(uint16_t evt_type) {
  return evt_type & (1 << BLE_EVT_CONNECTABLE_BIT);
}

static bool ble_evt_type_is_scannable(uint16_t evt_type) {
  return evt_type & (1 << BLE_EVT_SCANNABLE_BIT);
}

static bool ble_evt_type_is_directed(uint16_t evt_type) {
  return evt_type & (1 << BLE_EVT_DIRECTED_BIT);
}

static bool ble_evt_type_is_scan_resp(uint16_t evt_type) {
  return evt_type & (1 << BLE_EVT_SCAN_RESPONSE_BIT);
}

static bool ble_evt_type_is_legacy(uint16_t evt_type) {
  return evt_type & (1 << BLE_EVT_LEGACY_BIT);
}

static uint8_t ble_evt_type_data_status(uint16_t evt_type) {
  return (evt_type >> 5) & 3;
}

constexpr uint8_t UNSUPPORTED = 255;

/* LE states combo bit to check */
const uint8_t btm_le_state_combo_tbl[BTM_BLE_STATE_MAX][BTM_BLE_STATE_MAX] = {
    {
        /* single state support */
        HCI_LE_STATES_CONN_ADV_BIT,   /* conn_adv */
        HCI_LE_STATES_INIT_BIT,       /* init */
        HCI_LE_STATES_INIT_BIT,       /* central */
        HCI_LE_STATES_PERIPHERAL_BIT, /* peripheral */
        UNSUPPORTED,                  /* todo: lo du dir adv, not covered ? */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_BIT, /* hi duty dir adv */
        HCI_LE_STATES_NON_CONN_ADV_BIT,    /* non connectable adv */
        HCI_LE_STATES_PASS_SCAN_BIT,       /*  passive scan */
        HCI_LE_STATES_ACTIVE_SCAN_BIT,     /*   active scan */
        HCI_LE_STATES_SCAN_ADV_BIT         /* scanable adv */
    },
    {
        /* conn_adv =0 */
        UNSUPPORTED,                            /* conn_adv */
        HCI_LE_STATES_CONN_ADV_INIT_BIT,        /* init: 32 */
        HCI_LE_STATES_CONN_ADV_CENTRAL_BIT,     /* central: 35 */
        HCI_LE_STATES_CONN_ADV_PERIPHERAL_BIT,  /* peripheral: 38,*/
        UNSUPPORTED,                            /* lo du dir adv */
        UNSUPPORTED,                            /* hi duty dir adv */
        UNSUPPORTED,                            /* non connectable adv */
        HCI_LE_STATES_CONN_ADV_PASS_SCAN_BIT,   /*  passive scan */
        HCI_LE_STATES_CONN_ADV_ACTIVE_SCAN_BIT, /*   active scan */
        UNSUPPORTED                             /* scanable adv */
    },
    {
        /* init */
        HCI_LE_STATES_CONN_ADV_INIT_BIT,           /* conn_adv: 32 */
        UNSUPPORTED,                               /* init */
        HCI_LE_STATES_INIT_CENTRAL_BIT,            /* central 28 */
        HCI_LE_STATES_INIT_CENTRAL_PERIPHERAL_BIT, /* peripheral 41 */
        HCI_LE_STATES_LO_DUTY_DIR_ADV_INIT_BIT,    /* lo du dir adv 34 */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_INIT_BIT,    /* hi duty dir adv 33 */
        HCI_LE_STATES_NON_CONN_INIT_BIT,           /*  non connectable adv */
        HCI_LE_STATES_PASS_SCAN_INIT_BIT,          /* passive scan */
        HCI_LE_STATES_ACTIVE_SCAN_INIT_BIT,        /*  active scan */
        HCI_LE_STATES_SCAN_ADV_INIT_BIT            /* scanable adv */

    },
    {
        /* central */
        HCI_LE_STATES_CONN_ADV_CENTRAL_BIT,        /* conn_adv: 35 */
        HCI_LE_STATES_INIT_CENTRAL_BIT,            /* init 28 */
        HCI_LE_STATES_INIT_CENTRAL_BIT,            /* central 28 */
        HCI_LE_STATES_CONN_ADV_INIT_BIT,           /* peripheral: 32 */
        HCI_LE_STATES_LO_DUTY_DIR_ADV_CENTRAL_BIT, /* lo duty cycle adv 37 */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_CENTRAL_BIT, /* hi duty cycle adv 36 */
        HCI_LE_STATES_NON_CONN_ADV_CENTRAL_BIT,    /*  non connectable adv*/
        HCI_LE_STATES_PASS_SCAN_CENTRAL_BIT,       /*  passive scan */
        HCI_LE_STATES_ACTIVE_SCAN_CENTRAL_BIT,     /*   active scan */
        HCI_LE_STATES_SCAN_ADV_CENTRAL_BIT         /*  scanable adv */

    },
    {
        /* peripheral */
        HCI_LE_STATES_CONN_ADV_PERIPHERAL_BIT,        /* conn_adv: 38,*/
        HCI_LE_STATES_INIT_CENTRAL_PERIPHERAL_BIT,    /* init 41 */
        HCI_LE_STATES_INIT_CENTRAL_PERIPHERAL_BIT,    /* central 41 */
        HCI_LE_STATES_CONN_ADV_PERIPHERAL_BIT,        /* peripheral: 38,*/
        HCI_LE_STATES_LO_DUTY_DIR_ADV_PERIPHERAL_BIT, /* lo duty cycle adv 40 */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_PERIPHERAL_BIT, /* hi duty cycle adv 39 */
        HCI_LE_STATES_NON_CONN_ADV_PERIPHERAL_BIT,    /* non connectable adv */
        HCI_LE_STATES_PASS_SCAN_PERIPHERAL_BIT,       /* passive scan */
        HCI_LE_STATES_ACTIVE_SCAN_PERIPHERAL_BIT,     /*  active scan */
        HCI_LE_STATES_SCAN_ADV_PERIPHERAL_BIT         /* scanable adv */

    },
    {
        /* lo duty cycle adv */
        UNSUPPORTED,                                  /* conn_adv: 38,*/
        HCI_LE_STATES_LO_DUTY_DIR_ADV_INIT_BIT,       /* init 34 */
        HCI_LE_STATES_LO_DUTY_DIR_ADV_CENTRAL_BIT,    /* central 37 */
        HCI_LE_STATES_LO_DUTY_DIR_ADV_PERIPHERAL_BIT, /* peripheral: 40 */
        UNSUPPORTED,                                  /* lo duty cycle adv 40 */
        UNSUPPORTED,                                  /* hi duty cycle adv 39 */
        UNSUPPORTED,                                  /*  non connectable adv */
        UNSUPPORTED, /* TODO: passive scan, not covered? */
        UNSUPPORTED, /* TODO:  active scan, not covered? */
        UNSUPPORTED  /*  scanable adv */
    },
    {
        /* hi duty cycle adv */
        UNSUPPORTED,                                  /* conn_adv: 38,*/
        HCI_LE_STATES_HI_DUTY_DIR_ADV_INIT_BIT,       /* init 33 */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_CENTRAL_BIT,    /* central 36 */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_PERIPHERAL_BIT, /* peripheral: 39*/
        UNSUPPORTED,                                  /* lo duty cycle adv 40 */
        UNSUPPORTED,                                  /* hi duty cycle adv 39 */
        UNSUPPORTED,                                  /* non connectable adv */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_PASS_SCAN_BIT,  /* passive scan */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_ACTIVE_SCAN_BIT, /* active scan */
        UNSUPPORTED                                    /* scanable adv */
    },
    {
        /* non connectable adv */
        UNSUPPORTED,                                /* conn_adv: */
        HCI_LE_STATES_NON_CONN_INIT_BIT,            /* init  */
        HCI_LE_STATES_NON_CONN_ADV_CENTRAL_BIT,     /* central  */
        HCI_LE_STATES_NON_CONN_ADV_PERIPHERAL_BIT,  /* peripheral: */
        UNSUPPORTED,                                /* lo duty cycle adv */
        UNSUPPORTED,                                /* hi duty cycle adv */
        UNSUPPORTED,                                /* non connectable adv */
        HCI_LE_STATES_NON_CONN_ADV_PASS_SCAN_BIT,   /* passive scan */
        HCI_LE_STATES_NON_CONN_ADV_ACTIVE_SCAN_BIT, /* active scan */
        UNSUPPORTED                                 /* scanable adv */
    },
    {
        /* passive scan */
        HCI_LE_STATES_CONN_ADV_PASS_SCAN_BIT,        /* conn_adv: */
        HCI_LE_STATES_PASS_SCAN_INIT_BIT,            /* init  */
        HCI_LE_STATES_PASS_SCAN_CENTRAL_BIT,         /* central  */
        HCI_LE_STATES_PASS_SCAN_PERIPHERAL_BIT,      /* peripheral: */
        UNSUPPORTED,                                 /* lo duty cycle adv */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_PASS_SCAN_BIT, /* hi duty cycle adv */
        HCI_LE_STATES_NON_CONN_ADV_PASS_SCAN_BIT,    /* non connectable adv */
        UNSUPPORTED,                                 /* passive scan */
        UNSUPPORTED,                                 /* active scan */
        HCI_LE_STATES_SCAN_ADV_PASS_SCAN_BIT         /* scanable adv */
    },
    {
        /* active scan */
        HCI_LE_STATES_CONN_ADV_ACTIVE_SCAN_BIT,        /* conn_adv: */
        HCI_LE_STATES_ACTIVE_SCAN_INIT_BIT,            /* init  */
        HCI_LE_STATES_ACTIVE_SCAN_CENTRAL_BIT,         /* central  */
        HCI_LE_STATES_ACTIVE_SCAN_PERIPHERAL_BIT,      /* peripheral: */
        UNSUPPORTED,                                   /* lo duty cycle adv */
        HCI_LE_STATES_HI_DUTY_DIR_ADV_ACTIVE_SCAN_BIT, /* hi duty cycle adv */
        HCI_LE_STATES_NON_CONN_ADV_ACTIVE_SCAN_BIT, /*  non connectable adv */
        UNSUPPORTED,                                /* TODO: passive scan */
        UNSUPPORTED,                                /* TODO:  active scan */
        HCI_LE_STATES_SCAN_ADV_ACTIVE_SCAN_BIT      /*  scanable adv */
    },
    {
        /* scanable adv */
        UNSUPPORTED,                            /* conn_adv: */
        HCI_LE_STATES_SCAN_ADV_INIT_BIT,        /* init  */
        HCI_LE_STATES_SCAN_ADV_CENTRAL_BIT,     /* central  */
        HCI_LE_STATES_SCAN_ADV_PERIPHERAL_BIT,  /* peripheral: */
        UNSUPPORTED,                            /* lo duty cycle adv */
        UNSUPPORTED,                            /* hi duty cycle adv */
        UNSUPPORTED,                            /* non connectable adv */
        HCI_LE_STATES_SCAN_ADV_PASS_SCAN_BIT,   /*  passive scan */
        HCI_LE_STATES_SCAN_ADV_ACTIVE_SCAN_BIT, /*  active scan */
        UNSUPPORTED                             /* scanable adv */
    }};

/* check LE combo state supported */
inline bool BTM_LE_STATES_SUPPORTED(const uint8_t* x, uint8_t bit_num) {
  uint8_t mask = 1 << (bit_num % 8);
  uint8_t offset = bit_num / 8;
  return ((x)[offset] & mask);
}

void BTM_BleOpportunisticObserve(bool enable,
                                 tBTM_INQ_RESULTS_CB* p_results_cb) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    bluetooth::shim::BTM_BleOpportunisticObserve(enable, p_results_cb);
    // NOTE: passthrough, no return here. GD would send the results back to BTM,
    // and it needs the callbacks set properly.
  }

  if (enable) {
    btm_cb.ble_ctr_cb.p_opportunistic_obs_results_cb = p_results_cb;
  } else {
    btm_cb.ble_ctr_cb.p_opportunistic_obs_results_cb = NULL;
  }
}

/*******************************************************************************
 *
 * Function         BTM_BleObserve
 *
 * Description      This procedure keep the device listening for advertising
 *                  events from a broadcast device.
 *
 * Parameters       start: start or stop observe.
 *                  duration: how long the scan should last, in seconds. 0 means
 *                  scan without timeout. Starting the scan second time without
 *                  timeout will disable the timer.
 *
 * Returns          void
 *
 ******************************************************************************/
tBTM_STATUS BTM_BleObserve(bool start, uint8_t duration,
                           tBTM_INQ_RESULTS_CB* p_results_cb,
                           tBTM_CMPL_CB* p_cmpl_cb) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::BTM_BleObserve(start, duration, p_results_cb,
                                           p_cmpl_cb);
  }

  tBTM_BLE_INQ_CB* p_inq = &btm_cb.ble_ctr_cb.inq_var;
  tBTM_STATUS status = BTM_WRONG_MODE;

  uint32_t scan_interval =
      !p_inq->scan_interval ? BTM_BLE_GAP_DISC_SCAN_INT : p_inq->scan_interval;
  uint32_t scan_window =
      !p_inq->scan_window ? BTM_BLE_GAP_DISC_SCAN_WIN : p_inq->scan_window;

  BTM_TRACE_EVENT("%s : scan_type:%d, %d, %d", __func__, p_inq->scan_type,
                  p_inq->scan_interval, p_inq->scan_window);

  if (!controller_get_interface()->supports_ble()) return BTM_ILLEGAL_VALUE;

  if (start) {
    /* shared inquiry database, do not allow observe if any inquiry is active */
    if (btm_cb.ble_ctr_cb.is_ble_observe_active()) {
      if (duration == 0) {
        if (alarm_is_scheduled(btm_cb.ble_ctr_cb.observer_timer)) {
          alarm_cancel(btm_cb.ble_ctr_cb.observer_timer);
          return BTM_CMD_STARTED;
        }
      }
      BTM_TRACE_ERROR("%s Observe Already Active", __func__);
      return status;
    }

    btm_cb.ble_ctr_cb.p_obs_results_cb = p_results_cb;
    btm_cb.ble_ctr_cb.p_obs_cmpl_cb = p_cmpl_cb;
    status = BTM_CMD_STARTED;

    /* scan is not started */
    if (!btm_cb.ble_ctr_cb.is_ble_scan_active()) {
      /* allow config of scan type */
      cache.ClearAll();
      p_inq->scan_type = (p_inq->scan_type == BTM_BLE_SCAN_MODE_NONE)
                             ? BTM_BLE_SCAN_MODE_ACTI
                             : p_inq->scan_type;
      btm_send_hci_set_scan_params(
          p_inq->scan_type, (uint16_t)scan_interval, (uint16_t)scan_window,
          btm_cb.ble_ctr_cb.addr_mgnt_cb.own_addr_type, BTM_BLE_DEFAULT_SFP);

      btm_ble_start_scan();
    }

    if (status == BTM_CMD_STARTED) {
      btm_cb.ble_ctr_cb.set_ble_observe_active();
      if (duration != 0) {
        /* start observer timer */
        uint64_t duration_ms = duration * 1000;
        alarm_set_on_mloop(btm_cb.ble_ctr_cb.observer_timer, duration_ms,
                           btm_ble_observer_timer_timeout, NULL);
      }
    }
  } else if (btm_cb.ble_ctr_cb.is_ble_observe_active()) {
    status = BTM_CMD_STARTED;
    btm_ble_stop_observe();
  } else {
    BTM_TRACE_ERROR("%s Observe not active", __func__);
  }

  return status;
}

#if (BLE_VND_INCLUDED == TRUE)

static void btm_get_dynamic_audio_buffer_vsc_cmpl_cback(
    tBTM_VSC_CMPL* p_vsc_cmpl_params) {
  LOG(INFO) << __func__;

  if (p_vsc_cmpl_params->param_len < 1) {
    LOG(ERROR) << __func__
               << ": The length of returned parameters is less than 1";
    return;
  }
  uint8_t* p_event_param_buf = p_vsc_cmpl_params->p_param_buf;
  uint8_t status = 0xff;
  uint8_t opcode = 0xff;
  uint32_t codec_mask = 0xffffffff;

  // [Return Parameter]         | [Size]   | [Purpose]
  // Status                     | 1 octet  | Command complete status
  // Dynamic_Audio_Buffer_opcode| 1 octet  | 0x01 - Get buffer time
  // Audio_Codedc_Type_Supported| 4 octet  | Bit masks for selected codec types
  // Audio_Codec_Buffer_Time    | 192 octet| Default/Max/Min buffer time
  STREAM_TO_UINT8(status, p_event_param_buf);
  if (status != HCI_SUCCESS) {
    LOG(ERROR) << __func__
               << ": Fail to configure DFTB. status: " << loghex(status);
    return;
  }

  if (p_vsc_cmpl_params->param_len != 198) {
    LOG(FATAL) << __func__
               << ": The length of returned parameters is not equal to 198: "
               << std::to_string(p_vsc_cmpl_params->param_len);
    return;
  }

  STREAM_TO_UINT8(opcode, p_event_param_buf);
  LOG(INFO) << __func__ << ": opcode = " << loghex(opcode);

  if (opcode == 0x01) {
    STREAM_TO_UINT32(codec_mask, p_event_param_buf);
    LOG(INFO) << __func__ << ": codec_mask = " << loghex(codec_mask);

    for (int i = 0; i < BTM_CODEC_TYPE_MAX_RECORDS; i++) {
      STREAM_TO_UINT16(btm_cb.dynamic_audio_buffer_cb[i].default_buffer_time,
                       p_event_param_buf);
      STREAM_TO_UINT16(btm_cb.dynamic_audio_buffer_cb[i].maximum_buffer_time,
                       p_event_param_buf);
      STREAM_TO_UINT16(btm_cb.dynamic_audio_buffer_cb[i].minimum_buffer_time,
                       p_event_param_buf);
    }

    LOG(INFO) << __func__ << ": Succeed to receive Media Tx Buffer.";
  }
}

/*******************************************************************************
 *
 * Function         btm_vsc_brcm_features_complete
 *
 * Description      Command Complete callback for HCI_BLE_VENDOR_CAP
 *
 * Returns          void
 *
 ******************************************************************************/
static void btm_ble_vendor_capability_vsc_cmpl_cback(
    tBTM_VSC_CMPL* p_vcs_cplt_params) {
  BTM_TRACE_DEBUG("%s", __func__);

  /* Check status of command complete event */
  CHECK(p_vcs_cplt_params->opcode == HCI_BLE_VENDOR_CAP);
  CHECK(p_vcs_cplt_params->param_len > 0);

  const uint8_t* p = p_vcs_cplt_params->p_param_buf;
  uint8_t raw_status;
  STREAM_TO_UINT8(raw_status, p);
  tHCI_STATUS status = to_hci_status_code(raw_status);

  if (status != HCI_SUCCESS) {
    BTM_TRACE_DEBUG("%s: Status = 0x%02x (0 is success)", __func__, status);
    return;
  }

  if (p_vcs_cplt_params->param_len >= BTM_VSC_CHIP_CAPABILITY_RSP_LEN) {
    CHECK(p_vcs_cplt_params->param_len >= BTM_VSC_CHIP_CAPABILITY_RSP_LEN);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.adv_inst_max, p);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.rpa_offloading, p);
    STREAM_TO_UINT16(btm_cb.cmn_ble_vsc_cb.tot_scan_results_strg, p);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.max_irk_list_sz, p);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.filter_support, p);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.max_filter, p);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.energy_support, p);
  }

  if (p_vcs_cplt_params->param_len >
      BTM_VSC_CHIP_CAPABILITY_RSP_LEN_L_RELEASE) {
    STREAM_TO_UINT16(btm_cb.cmn_ble_vsc_cb.version_supported, p);
  } else {
    btm_cb.cmn_ble_vsc_cb.version_supported = BTM_VSC_CHIP_CAPABILITY_L_VERSION;
  }

  if (btm_cb.cmn_ble_vsc_cb.version_supported >=
      BTM_VSC_CHIP_CAPABILITY_M_VERSION) {
    CHECK(p_vcs_cplt_params->param_len >=
          BTM_VSC_CHIP_CAPABILITY_RSP_LEN_M_RELEASE);
    STREAM_TO_UINT16(btm_cb.cmn_ble_vsc_cb.total_trackable_advertisers, p);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.extended_scan_support, p);
    STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.debug_logging_supported, p);
  }

  if (btm_cb.cmn_ble_vsc_cb.version_supported >=
      BTM_VSC_CHIP_CAPABILITY_S_VERSION) {
    if (p_vcs_cplt_params->param_len >=
        BTM_VSC_CHIP_CAPABILITY_RSP_LEN_S_RELEASE) {
      STREAM_TO_UINT8(
          btm_cb.cmn_ble_vsc_cb.le_address_generation_offloading_support, p);
      STREAM_TO_UINT32(
          btm_cb.cmn_ble_vsc_cb.a2dp_source_offload_capability_mask, p);
      STREAM_TO_UINT8(btm_cb.cmn_ble_vsc_cb.quality_report_support, p);
      STREAM_TO_UINT32(btm_cb.cmn_ble_vsc_cb.dynamic_audio_buffer_support, p);

      if (btm_cb.cmn_ble_vsc_cb.dynamic_audio_buffer_support != 0) {
        uint8_t param[3] = {0};
        uint8_t* p_param = param;

        UINT8_TO_STREAM(p_param, HCI_CONTROLLER_DAB_GET_BUFFER_TIME);
        BTM_VendorSpecificCommand(HCI_CONTROLLER_DAB, p_param - param, param,
                                  btm_get_dynamic_audio_buffer_vsc_cmpl_cback);
      }
    }
  }
  btm_cb.cmn_ble_vsc_cb.values_read = true;

  BTM_TRACE_DEBUG(
      "%s: stat=%d, irk=%d, ADV ins:%d, rpa=%d, ener=%d, ext_scan=%d", __func__,
      status, btm_cb.cmn_ble_vsc_cb.max_irk_list_sz,
      btm_cb.cmn_ble_vsc_cb.adv_inst_max, btm_cb.cmn_ble_vsc_cb.rpa_offloading,
      btm_cb.cmn_ble_vsc_cb.energy_support,
      btm_cb.cmn_ble_vsc_cb.extended_scan_support);

  btm_ble_adv_init();

  if (btm_cb.cmn_ble_vsc_cb.max_filter > 0) btm_ble_adv_filter_init();

  /* VS capability included and non-4.2 device */
  if (controller_get_interface()->supports_ble() && 
      controller_get_interface()->supports_ble_privacy() &&
      btm_cb.cmn_ble_vsc_cb.max_irk_list_sz > 0 &&
      controller_get_interface()->get_ble_resolving_list_max_size() == 0)
    btm_ble_resolving_list_init(btm_cb.cmn_ble_vsc_cb.max_irk_list_sz);

  if (btm_cb.cmn_ble_vsc_cb.tot_scan_results_strg > 0) btm_ble_batchscan_init();

  if (p_ctrl_le_feature_rd_cmpl_cback != NULL)
    p_ctrl_le_feature_rd_cmpl_cback(static_cast<tHCI_STATUS>(status));
}
#endif /* (BLE_VND_INCLUDED == TRUE) */

/*******************************************************************************
 *
 * Function         BTM_BleGetVendorCapabilities
 *
 * Description      This function reads local LE features
 *
 * Parameters       p_cmn_vsc_cb : Locala LE capability structure
 *
 * Returns          void
 *
 ******************************************************************************/
void BTM_BleGetVendorCapabilities(tBTM_BLE_VSC_CB* p_cmn_vsc_cb) {
  if (NULL != p_cmn_vsc_cb) {
    *p_cmn_vsc_cb = btm_cb.cmn_ble_vsc_cb;
  }
}

void BTM_BleGetDynamicAudioBuffer(
    tBTM_BT_DYNAMIC_AUDIO_BUFFER_CB p_dynamic_audio_buffer_cb[]) {
  BTM_TRACE_DEBUG("BTM_BleGetDynamicAudioBuffer");

  if (NULL != p_dynamic_audio_buffer_cb) {
    for (int i = 0; i < 32; i++) {
      p_dynamic_audio_buffer_cb[i] = btm_cb.dynamic_audio_buffer_cb[i];
    }
  }
}

/******************************************************************************
 *
 * Function         BTM_BleReadControllerFeatures
 *
 * Description      Reads BLE specific controller features
 *
 * Parameters:      tBTM_BLE_CTRL_FEATURES_CBACK : Callback to notify when
 *                  features are read
 *
 * Returns          void
 *
 ******************************************************************************/
#if (BLE_VND_INCLUDED == TRUE)
void BTM_BleReadControllerFeatures(tBTM_BLE_CTRL_FEATURES_CBACK* p_vsc_cback) {
  if (btm_cb.cmn_ble_vsc_cb.values_read) return;

  BTM_TRACE_DEBUG("BTM_BleReadControllerFeatures");

  p_ctrl_le_feature_rd_cmpl_cback = p_vsc_cback;
  BTM_VendorSpecificCommand(HCI_BLE_VENDOR_CAP, 0, NULL,
                            btm_ble_vendor_capability_vsc_cmpl_cback);
}
#else
void BTM_BleReadControllerFeatures(
    UNUSED_ATTR tBTM_BLE_CTRL_FEATURES_CBACK* p_vsc_cback) {}
#endif

/*******************************************************************************
 *
 * Function         BTM_BleConfigPrivacy
 *
 * Description      This function is called to enable or disable the privacy in
 *                   LE channel of the local device.
 *
 * Parameters       privacy_mode:  privacy mode on or off.
 *
 * Returns          bool    privacy mode set success; otherwise failed.
 *
 ******************************************************************************/
bool BTM_BleConfigPrivacy(bool privacy_mode) {
  tBTM_BLE_CB* p_cb = &btm_cb.ble_ctr_cb;

  BTM_TRACE_WARNING("%s %d", __func__, (int)privacy_mode);

  /* if LE is not supported, return error */
  if (!controller_get_interface()->supports_ble()) return false;

  tGAP_BLE_ATTR_VALUE gap_ble_attr_value;
  gap_ble_attr_value.addr_resolution = 0;
  if (!privacy_mode) /* if privacy disabled, always use public address */
  {
    p_cb->addr_mgnt_cb.own_addr_type = BLE_ADDR_PUBLIC;
    p_cb->privacy_mode = BTM_PRIVACY_NONE;
  } else /* privacy is turned on*/
  {
    /* always set host random address, used when privacy 1.1 or priavcy 1.2 is
     * disabled */
    p_cb->addr_mgnt_cb.own_addr_type = BLE_ADDR_RANDOM;
    btm_gen_resolvable_private_addr(base::Bind(&btm_gen_resolve_paddr_low));

    /* 4.2 controller only allow privacy 1.2 or mixed mode, resolvable private
     * address in controller */
    if (controller_get_interface()->supports_ble_privacy()) {
      gap_ble_attr_value.addr_resolution = 1;
      p_cb->privacy_mode = BTM_PRIVACY_1_2;
    } else /* 4.1/4.0 controller */
      p_cb->privacy_mode = BTM_PRIVACY_1_1;
  }
  VLOG(2) << __func__ << " privacy_mode: " << p_cb->privacy_mode
          << " own_addr_type: " << p_cb->addr_mgnt_cb.own_addr_type;

  GAP_BleAttrDBUpdate(GATT_UUID_GAP_CENTRAL_ADDR_RESOL, &gap_ble_attr_value);

  bluetooth::shim::ACL_ConfigureLePrivacy(privacy_mode);
  return true;
}

/*******************************************************************************
 *
 * Function          BTM_BleMaxMultiAdvInstanceCount
 *
 * Description        Returns max number of multi adv instances supported by
 *                  controller
 *
 * Returns          Max multi adv instance count
 *
 ******************************************************************************/
uint8_t BTM_BleMaxMultiAdvInstanceCount(void) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::BTM_BleMaxMultiAdvInstanceCount();
  }
  return btm_cb.cmn_ble_vsc_cb.adv_inst_max < BTM_BLE_MULTI_ADV_MAX
             ? btm_cb.cmn_ble_vsc_cb.adv_inst_max
             : BTM_BLE_MULTI_ADV_MAX;
}

/*******************************************************************************
 *
 * Function         BTM_BleLocalPrivacyEnabled
 *
 * Description        Checks if local device supports private address
 *
 * Returns          Return true if local privacy is enabled else false
 *
 ******************************************************************************/
bool BTM_BleLocalPrivacyEnabled(void) {
  if (bluetooth::shim::is_gd_shim_enabled()) {
    return bluetooth::shim::BTM_BleLocalPrivacyEnabled();
  }
  return (btm_cb.ble_ctr_cb.privacy_mode != BTM_PRIVACY_NONE);
}

static bool is_resolving_list_bit_set(void* data, void* context) {
  tBTM_SEC_DEV_REC* p_dev_rec = static_cast<tBTM_SEC_DEV_REC*>(data);

  if ((p_dev_rec->ble.in_controller_list & BTM_RESOLVING_LIST_BIT) != 0)
    return false;

  return true;
}

/*******************************************************************************
 * PAST and Periodic Sync helper functions
 ******************************************************************************/

static void sync_queue_add(sync_node_t* p_param) {
  std::unique_lock<std::mutex> guard(sync_queue_mutex_);
  if (!sync_queue) {
    LOG_INFO("%s: allocating sync queue", __func__);
    sync_queue = list_new(osi_free);
    CHECK(sync_queue != NULL);
  }

  // Validity check
  CHECK(list_length(sync_queue) < MAX_SYNC_TRANSACTION);
  sync_node_t* p_node = (sync_node_t*)osi_malloc(sizeof(sync_node_t));
  *p_node = *p_param;
  list_append(sync_queue, p_node);
}

static void sync_queue_advance() {
  LOG_DEBUG("%s", "");
  std::unique_lock<std::mutex> guard(sync_queue_mutex_);

  if (sync_queue && !list_is_empty(sync_queue)) {
    sync_node_t* p_head = (sync_node_t*)list_front(sync_queue);
    LOG_INFO("queue_advance");
    list_remove(sync_queue, p_head);
  }
}

static void sync_queue_cleanup(remove_sync_node_t* p_param) {
  std::unique_lock<std::mutex> guard(sync_queue_mutex_);
  if (!sync_queue) {
    return;
  }

  sync_node_t* sync_request;
  const list_node_t* node = list_begin(sync_queue);
  while (node && node != list_end(sync_queue)) {
    sync_request = (sync_node_t*)list_node(node);
    node = list_next(node);
    if (sync_request->sid == p_param->sid &&
        sync_request->address == p_param->address) {
      LOG_INFO("%s: removing connection request SID=%04X, bd_addr=%s, busy=%d",
               __func__, sync_request->sid,
               sync_request->address.ToString().c_str(), sync_request->busy);
      list_remove(sync_queue, sync_request);
    }
  }
}

void btm_ble_start_sync_request(uint8_t sid, RawAddress addr, uint16_t skip,
                                uint16_t timeout) {
  tBLE_ADDR_TYPE address_type = BLE_ADDR_RANDOM;
  tINQ_DB_ENT* p_i = btm_inq_db_find(addr);
  if (p_i) {
    address_type = p_i->inq_info.results.ble_addr_type;  // Random
  }
  btm_random_pseudo_to_identity_addr(&addr, &address_type);
  address_type &= ~BLE_ADDR_TYPE_ID_BIT;
  uint8_t options = 0;
  uint8_t cte_type = 7;
  int index = btm_ble_get_psync_index(sid, addr);
  tBTM_BLE_PERIODIC_SYNC* p = &btm_ble_pa_sync_cb.p_sync[index];
  p->sync_state = PERIODIC_SYNC_PENDING;

  if (BleScanningManager::IsInitialized()) {
    BleScanningManager::Get()->PeriodicScanStart(options, sid, address_type,
                                                 addr, skip, timeout, cte_type);
  }

  alarm_set(sync_timeout_alarm, SYNC_TIMEOUT, btm_ble_start_sync_timeout, NULL);
}

static void btm_queue_sync_next() {
  if (!sync_queue || list_is_empty(sync_queue)) {
    LOG_DEBUG("sync_queue empty");
    return;
  }

  sync_node_t* p_head = (sync_node_t*)list_front(sync_queue);

  LOG_INFO("%s: executing sync request SID=%04X, bd_addr=%s", __func__,
           p_head->sid, p_head->address.ToString().c_str());
  if (p_head->busy) {
    LOG_DEBUG("BUSY");
    return;
  }

  p_head->busy = true;
  alarm_cancel(sync_timeout_alarm);
  btm_ble_start_sync_request(p_head->sid, p_head->address, p_head->skip,
                             p_head->timeout);
}

static void btm_ble_sync_queue_handle(uint16_t event, char* param) {
  switch (event) {
    case BTM_QUEUE_SYNC_REQ_EVT:
      LOG_DEBUG("BTIF_QUEUE_SYNC_REQ_EVT");
      sync_queue_add((sync_node_t*)param);
      break;
    case BTM_QUEUE_SYNC_ADVANCE_EVT:
      LOG_DEBUG("BTIF_QUEUE_ADVANCE_EVT");
      sync_queue_advance();
      break;
    case BTM_QUEUE_SYNC_CLEANUP_EVT:
      sync_queue_cleanup((remove_sync_node_t*)param);
      return;
  }
  btm_queue_sync_next();
}

void btm_queue_start_sync_req(uint8_t sid, RawAddress address, uint16_t skip,
                              uint16_t timeout) {
  LOG_DEBUG("address = %s, sid = %d", address.ToString().c_str(), sid);
  sync_node_t node = {};
  node.sid = sid;
  node.address = address;
  node.skip = skip;
  node.timeout = timeout;
  btm_ble_sync_queue_handle(BTM_QUEUE_SYNC_REQ_EVT, (char*)&node);
}

static void btm_sync_queue_advance() {
  LOG_DEBUG("%s", "");
  btm_ble_sync_queue_handle(BTM_QUEUE_SYNC_ADVANCE_EVT, nullptr);
}

static void btm_ble_start_sync_timeout(void* data) {
  LOG_DEBUG("%s", "");
  sync_node_t* p_head = (sync_node_t*)list_front(sync_queue);
  uint8_t adv_sid = p_head->sid;
  RawAddress address = p_head->address;

  int index = btm_ble_get_psync_index(adv_sid, address);

  tBTM_BLE_PERIODIC_SYNC* p = &btm_ble_pa_sync_cb.p_sync[index];

  if (BleScanningManager::IsInitialized()) {
    BleScanningManager::Get()->PeriodicScanCancelStart();
  }
  p->sync_start_cb.Run(0x3C, 0, p->sid, 0, p->remote_bda, 0, 0);

  p->sync_state = PERIODIC_SYNC_IDLE;
  p->in_use = false;
  p->remote_bda = RawAddress::kEmpty;
  p->sid = 0;
  p->sync_handle = 0;
  p->in_use = false;
}

static int btm_ble_get_free_psync_index() {
  int i;
  for (i = 0; i < MAX_SYNC_TRANSACTION; i++) {
    if (btm_ble_pa_sync_cb.p_sync[i].in_use == false) {
      LOG_DEBUG("found index at %d", i);
      return i;
    }
  }
  return i;
}

static int btm_ble_get_psync_index_from_handle(uint16_t handle) {
  int i;
  for (i = 0; i < MAX_SYNC_TRANSACTION; i++) {
    if (btm_ble_pa_sync_cb.p_sync[i].sync_handle == handle &&
        btm_ble_pa_sync_cb.p_sync[i].sync_state == PERIODIC_SYNC_ESTABLISHED) {
      LOG_DEBUG("found index at %d", i);
      return i;
    }
  }
  return i;
}

static int btm_ble_get_psync_index(uint8_t adv_sid, RawAddress addr) {
  int i;
  for (i = 0; i < MAX_SYNC_TRANSACTION; i++) {
    if (btm_ble_pa_sync_cb.p_sync[i].sid == adv_sid &&
        btm_ble_pa_sync_cb.p_sync[i].remote_bda == addr) {
      LOG_DEBUG("found index at %d", i);
      return i;
    }
  }
  return i;
}

static int btm_ble_get_free_sync_transfer_index() {
  int i;
  for (i = 0; i < MAX_SYNC_TRANSACTION; i++) {
    if (!btm_ble_pa_sync_cb.sync_transfer[i].in_use) {
      LOG_DEBUG("found index at %d", i);
      return i;
    }
  }
  return i;
}

static int btm_ble_get_sync_transfer_index(uint16_t conn_handle) {
  int i;
  for (i = 0; i < MAX_SYNC_TRANSACTION; i++) {
    if (btm_ble_pa_sync_cb.sync_transfer[i].conn_handle == conn_handle) {
      LOG_DEBUG("found index at %d", i);
      return i;
    }
  }
  return i;
}

/*******************************************************************************
 *
 * Function         btm_ble_periodic_adv_sync_established
 *
 * Description      Periodic Adv Sync Established callback from controller when
 &                  sync to PA is established
 *
 *
 ******************************************************************************/
void btm_ble_periodic_adv_sync_established(uint8_t status, uint16_t sync_handle,
                                           uint8_t adv_sid,
                                           uint8_t address_type,
                                           const RawAddress& addr, uint8_t phy,
                                           uint16_t interval,
                                           uint8_t adv_clock_accuracy) {
  LOG_DEBUG(
      "[PSync]: status=%d, sync_handle=%d, s_id=%d, "
      "addr_type=%d, adv_phy=%d,adv_interval=%d, clock_acc=%d",
      status, sync_handle, adv_sid, address_type, phy, interval,
      adv_clock_accuracy);

  /*if (param_len != ADV_SYNC_ESTB_EVT_LEN) {
    BTM_TRACE_ERROR("[PSync]%s: Invalid event length",__func__);
    STREAM_TO_UINT8(status, param);
    if (status == BTM_SUCCESS) {
      STREAM_TO_UINT16(sync_handle, param);
      //btsnd_hcic_ble_terminate_periodic_sync(sync_handle);
      if (BleScanningManager::IsInitialized()) {
        BleScanningManager::Get()->PeriodicScanTerminate(sync_handle);
      }
      return;
    }
  }*/

  RawAddress bda = addr;
  alarm_cancel(sync_timeout_alarm);

  tBLE_ADDR_TYPE ble_addr_type = to_ble_addr_type(address_type);
  if (ble_addr_type & BLE_ADDR_TYPE_ID_BIT) {
    btm_identity_addr_to_random_pseudo(&bda, &ble_addr_type, true);
  }
  int index = btm_ble_get_psync_index(adv_sid, bda);
  if (index == MAX_SYNC_TRANSACTION) {
    BTM_TRACE_WARNING("[PSync]%s: Invalid index for sync established",
                      __func__);
    if (status == BTM_SUCCESS) {
      BTM_TRACE_WARNING("%s: Terminate sync", __func__);
      if (BleScanningManager::IsInitialized()) {
        BleScanningManager::Get()->PeriodicScanTerminate(sync_handle);
      }
    }
    btm_sync_queue_advance();
    return;
  }
  tBTM_BLE_PERIODIC_SYNC* ps = &btm_ble_pa_sync_cb.p_sync[index];
  ps->sync_handle = sync_handle;
  ps->sync_state = PERIODIC_SYNC_ESTABLISHED;
  ps->sync_start_cb.Run(status, sync_handle, adv_sid,
                        from_ble_addr_type(ble_addr_type), bda, phy, interval);
  btm_sync_queue_advance();
}

/*******************************************************************************
 *
 * Function        btm_ble_periodic_adv_report
 *
 * Description     This callback is received when controller estalishes sync
 *                 to a PA requested from host
 *
 ******************************************************************************/
void btm_ble_periodic_adv_report(uint16_t sync_handle, uint8_t tx_power,
                                 int8_t rssi, uint8_t cte_type,
                                 uint8_t data_status, uint8_t data_len,
                                 const uint8_t* periodic_data) {
  LOG_DEBUG(
      "[PSync]: sync_handle = %u, tx_power = %d, rssi = %d,"
      "cte_type = %u, data_status = %u, data_len = %u",
      sync_handle, tx_power, rssi, cte_type, data_status, data_len);

  std::vector<uint8_t> data;
  for (int i = 0; i < data_len; i++) {
    data.push_back(periodic_data[i]);
  }
  int index = btm_ble_get_psync_index_from_handle(sync_handle);
  if (index == MAX_SYNC_TRANSACTION) {
    LOG_ERROR("[PSync]: index not found for handle %u", sync_handle);
    return;
  }
  tBTM_BLE_PERIODIC_SYNC* ps = &btm_ble_pa_sync_cb.p_sync[index];
  LOG_DEBUG("%s", "[PSync]: invoking callback");
  ps->sync_report_cb.Run(sync_handle, tx_power, rssi, data_status, data);
}

/*******************************************************************************
 *
 * Function        btm_ble_periodic_adv_sync_lost
 *
 * Description     This callback is received when sync to PA is lost
 *
 ******************************************************************************/
void btm_ble_periodic_adv_sync_lost(uint16_t sync_handle) {
  LOG_DEBUG("[PSync]: sync_handle = %d", sync_handle);

  int index = btm_ble_get_psync_index_from_handle(sync_handle);
  tBTM_BLE_PERIODIC_SYNC* ps = &btm_ble_pa_sync_cb.p_sync[index];
  ps->sync_lost_cb.Run(sync_handle);

  ps->in_use = false;
  ps->sid = 0;
  ps->sync_handle = 0;
  ps->sync_state = PERIODIC_SYNC_IDLE;
  ps->remote_bda = RawAddress::kEmpty;
}

/*******************************************************************************
 *
 * Function        BTM_BleStartPeriodicSync
 *
 * Description     Create sync request to PA associated with address and sid
 *
 ******************************************************************************/
void BTM_BleStartPeriodicSync(uint8_t adv_sid, RawAddress address,
                              uint16_t skip, uint16_t timeout,
                              StartSyncCb syncCb, SyncReportCb reportCb,
                              SyncLostCb lostCb) {
  LOG_DEBUG("%s", "[PSync]");
  int index = btm_ble_get_free_psync_index();
  tBTM_BLE_PERIODIC_SYNC* p = &btm_ble_pa_sync_cb.p_sync[index];
  if (index == MAX_SYNC_TRANSACTION) {
    syncCb.Run(BTM_NO_RESOURCES, 0, adv_sid, BLE_ADDR_RANDOM, address, 0, 0);
    return;
  }
  p->in_use = true;
  p->remote_bda = address;
  p->sid = adv_sid;
  p->sync_start_cb = syncCb;
  p->sync_report_cb = reportCb;
  p->sync_lost_cb = lostCb;
  btm_queue_start_sync_req(adv_sid, address, skip, timeout);
}

/*******************************************************************************
 *
 * Function        BTM_BleStopPeriodicSync
 *
 * Description     Terminate sync request to PA associated with sync handle
 *
 ******************************************************************************/
void BTM_BleStopPeriodicSync(uint16_t handle) {
  LOG_DEBUG("[PSync]: handle = %u", handle);
  int index = btm_ble_get_psync_index_from_handle(handle);
  if (index == MAX_SYNC_TRANSACTION) {
    LOG_ERROR("[PSync]: invalid index for handle %u", handle);
    if (BleScanningManager::IsInitialized()) {
      BleScanningManager::Get()->PeriodicScanTerminate(handle);
    }
    return;
  }
  tBTM_BLE_PERIODIC_SYNC* p = &btm_ble_pa_sync_cb.p_sync[index];
  p->sync_state = PERIODIC_SYNC_IDLE;
  p->in_use = false;
  p->remote_bda = RawAddress::kEmpty;
  p->sid = 0;
  p->sync_handle = 0;
  p->in_use = false;
  if (BleScanningManager::IsInitialized()) {
    BleScanningManager::Get()->PeriodicScanTerminate(handle);
  }
}

/*******************************************************************************
 *
 * Function        BTM_BleCancelPeriodicSync
 *
 * Description     Cancel create sync request to PA associated with sid and
 *                 address
 *
 ******************************************************************************/
void BTM_BleCancelPeriodicSync(uint8_t adv_sid, RawAddress address) {
  LOG_DEBUG("%s", "[PSync]");
  int index = btm_ble_get_psync_index(adv_sid, address);
  if (index == MAX_SYNC_TRANSACTION) {
    LOG_ERROR("[PSync]:Invalid index for sid=%u", adv_sid);
    return;
  }
  tBTM_BLE_PERIODIC_SYNC* p = &btm_ble_pa_sync_cb.p_sync[index];
  if (p->sync_state == PERIODIC_SYNC_PENDING) {
    LOG_WARN("[PSync]: Sync state is pending for index %d", index);
    if (BleScanningManager::IsInitialized()) {
      BleScanningManager::Get()->PeriodicScanCancelStart();
    }
  } else if (p->sync_state == PERIODIC_SYNC_IDLE) {
    LOG_DEBUG("[PSync]: Removing Sync request from queue for index %d", index);
    remove_sync_node_t remove_node;
    remove_node.sid = adv_sid;
    remove_node.address = address;
    btm_ble_sync_queue_handle(BTM_QUEUE_SYNC_CLEANUP_EVT, (char*)&remove_node);
  }
  p->sync_state = PERIODIC_SYNC_IDLE;
  p->in_use = false;
  p->remote_bda = RawAddress::kEmpty;
  p->sid = 0;
  p->sync_handle = 0;
  p->in_use = false;
}

/*******************************************************************************
 *
 * Function        btm_ble_periodic_syc_transfer_cmd_cmpl
 *
 * Description     PAST complete callback
 *
 ******************************************************************************/
void btm_ble_periodic_syc_transfer_cmd_cmpl(uint8_t status,
                                            uint16_t conn_handle) {
  LOG_DEBUG("[PAST]: status = %d, conn_handle =%d", status, conn_handle);

  int index = btm_ble_get_sync_transfer_index(conn_handle);
  if (index == MAX_SYNC_TRANSACTION) {
    LOG_ERROR("[PAST]:Invalid, conn_handle %u not found in DB", conn_handle);
    return;
  }

  tBTM_BLE_PERIODIC_SYNC_TRANSFER* p_sync_transfer =
      &btm_ble_pa_sync_cb.sync_transfer[index];
  p_sync_transfer->cb.Run(status, p_sync_transfer->addr);

  p_sync_transfer->in_use = false;
  p_sync_transfer->conn_handle = -1;
  p_sync_transfer->addr = RawAddress::kEmpty;
}

void btm_ble_periodic_syc_transfer_param_cmpl(uint8_t status) {
  LOG_DEBUG("[PAST]: status = %d", status);
}

/*******************************************************************************
 *
 * Function        BTM_BlePeriodicSyncTransfer
 *
 * Description     Initiate PAST to connected remote device with sync handle
 *
 ******************************************************************************/
void BTM_BlePeriodicSyncTransfer(RawAddress addr, uint16_t service_data,
                                 uint16_t sync_handle, SyncTransferCb cb) {
  uint16_t conn_handle = BTM_GetHCIConnHandle(addr, BT_TRANSPORT_LE);
  tACL_CONN* p_acl = btm_acl_for_bda(addr, BT_TRANSPORT_LE);
  BTM_TRACE_DEBUG("[PAST]%s for connection_handle = %x", __func__, conn_handle);
  if (conn_handle == 0xFFFF || p_acl == NULL) {
    BTM_TRACE_ERROR("[PAST]%s:Invalid connection handle or no LE ACL link",
                    __func__);
    cb.Run(BTM_UNKNOWN_ADDR, addr);
    return;
  }

  if (!HCI_LE_PERIODIC_ADVERTISING_SYNC_TRANSFER_RECIPIENT(
          p_acl->peer_le_features)) {
    BTM_TRACE_ERROR("[PAST]%s:Remote doesn't support PAST", __func__);
    cb.Run(BTM_MODE_UNSUPPORTED, addr);
    return;
  }

  int index = btm_ble_get_free_sync_transfer_index();
  tBTM_BLE_PERIODIC_SYNC_TRANSFER* p_sync_transfer =
      &btm_ble_pa_sync_cb.sync_transfer[index];
  p_sync_transfer->in_use = true;
  p_sync_transfer->conn_handle = conn_handle;
  p_sync_transfer->addr = addr;
  p_sync_transfer->cb = cb;
  if (BleScanningManager::IsInitialized()) {
    BleScanningManager::Get()->PeriodicAdvSyncTransfer(
        addr, service_data, sync_handle,
        base::Bind(&btm_ble_periodic_syc_transfer_cmd_cmpl));
  }
}

/*******************************************************************************
 *
 * Function        BTM_BlePeriodicSyncSetInfo
 *
 * Description     Initiate PAST to connected remote device with adv handle
 *
 ******************************************************************************/
void BTM_BlePeriodicSyncSetInfo(RawAddress addr, uint16_t service_data,
                                uint8_t adv_handle, SyncTransferCb cb) {
  uint16_t conn_handle = BTM_GetHCIConnHandle(addr, BT_TRANSPORT_LE);
  tACL_CONN* p_acl = btm_acl_for_bda(addr, BT_TRANSPORT_LE);
  LOG_DEBUG("[PAST] for connection_handle = %u", conn_handle);
  if (conn_handle == 0xFFFF || p_acl == nullptr) {
    LOG_ERROR("[PAST]:Invalid connection handle %u or no LE ACL link",
              conn_handle);
    cb.Run(BTM_UNKNOWN_ADDR, addr);
    return;
  }
  if (!HCI_LE_PERIODIC_ADVERTISING_SYNC_TRANSFER_RECIPIENT(
          p_acl->peer_le_features)) {
    LOG_ERROR("%s", "[PAST]:Remote doesn't support PAST");
    cb.Run(BTM_MODE_UNSUPPORTED, addr);
    return;
  }

  int index = btm_ble_get_free_sync_transfer_index();
  tBTM_BLE_PERIODIC_SYNC_TRANSFER* p_sync_transfer =
      &btm_ble_pa_sync_cb.sync_transfer[index];
  p_sync_transfer->in_use = true;
  p_sync_transfer->conn_handle = conn_handle;
  p_sync_transfer->addr = addr;
  p_sync_transfer->cb = cb;
  if (BleScanningManager::IsInitialized()) {
    BleScanningManager::Get()->PeriodicAdvSetInfoTransfer(
        addr, service_data, adv_handle,
        base::Bind(&btm_ble_periodic_syc_transfer_cmd_cmpl));
  }
}

/*******************************************************************************
 *
 * Function        btm_ble_biginfo_adv_report_rcvd
 *
 * Description     Host receives this event when synced PA has BIGInfo
 *
 ******************************************************************************/
void btm_ble_biginfo_adv_report_rcvd(uint8_t* p, uint16_t param_len) {
  LOG_DEBUG("[PAST]: BIGINFO report received, len=%u", param_len);
  uint16_t sync_handle, iso_interval, max_pdu, max_sdu;
  uint8_t num_bises, nse, bn, pto, irc, phy, framing, encryption;
  uint32_t sdu_interval;
  STREAM_TO_UINT16(sync_handle, p);
  STREAM_TO_UINT8(num_bises, p);
  STREAM_TO_UINT8(nse, p);
  STREAM_TO_UINT16(iso_interval, p);
  STREAM_TO_UINT8(bn, p);
  STREAM_TO_UINT8(pto, p);
  STREAM_TO_UINT8(irc, p);
  STREAM_TO_UINT16(max_pdu, p);
  STREAM_TO_UINT24(sdu_interval, p);
  STREAM_TO_UINT16(max_sdu, p);
  STREAM_TO_UINT8(phy, p);
  STREAM_TO_UINT8(framing, p);
  STREAM_TO_UINT8(encryption, p);
  LOG_DEBUG(
      "[PAST]:sync_handle %u, num_bises = %u, nse = %u,"
      "iso_interval = %d, bn = %u, pto = %u, irc = %u, max_pdu = %u "
      "sdu_interval = %d, max_sdu = %u, phy = %u, framing = %u, encryption  = "
      "%u",
      sync_handle, num_bises, nse, iso_interval, bn, pto, irc, max_pdu,
      sdu_interval, max_sdu, phy, framing, encryption);
}

/*******************************************************************************
 *
 * Function        btm_ble_periodic_adv_sync_tx_rcvd
 *
 * Description     Host receives this event when the controller receives sync
 *                 info of PA from the connected remote device and successfully
 *                 synced to PA associated with sync handle
 *
 ******************************************************************************/
void btm_ble_periodic_adv_sync_tx_rcvd(uint8_t* p, uint16_t param_len) {
  LOG_DEBUG("[PAST]: PAST received, param_len=%u", param_len);
  if (param_len == 0) {
    LOG_ERROR("%s", "Insufficient data");
    return;
  }
  uint8_t status, adv_sid, address_type, adv_phy, clk_acc;
  uint16_t pa_int, sync_handle, service_data, conn_handle;
  RawAddress addr;
  STREAM_TO_UINT8(status, p);
  STREAM_TO_UINT16(conn_handle, p);
  STREAM_TO_UINT16(service_data, p);
  STREAM_TO_UINT16(sync_handle, p);
  STREAM_TO_UINT8(adv_sid, p);
  STREAM_TO_UINT8(address_type, p);
  STREAM_TO_BDADDR(addr, p);
  STREAM_TO_UINT8(adv_phy, p);
  STREAM_TO_UINT16(pa_int, p);
  STREAM_TO_UINT8(clk_acc, p);
  BTM_TRACE_DEBUG(
      "[PAST]: status = %u, conn_handle = %u, service_data = %u,"
      " sync_handle = %u, adv_sid = %u, address_type = %u, addr = %s,"
      " adv_phy = %u, pa_int = %u, clk_acc = %u",
      status, conn_handle, service_data, sync_handle, adv_sid, address_type,
      addr.ToString().c_str(), adv_phy, pa_int, clk_acc);
  if (syncRcvdCbRegistered) {
    sync_rcvd_cb.Run(status, sync_handle, adv_sid, address_type, addr, adv_phy,
                     pa_int);
  }
}

/*******************************************************************************
 *
 * Function        BTM_BlePeriodicSyncTxParameters
 *
 * Description     On receiver side this command is used to specify how BT SoC
 *                 will process PA sync info received from the remote device
 *                 identified by the addr.
 *
 ******************************************************************************/
void BTM_BlePeriodicSyncTxParameters(RawAddress addr, uint8_t mode,
                                     uint16_t skip, uint16_t timeout,
                                     StartSyncCb syncCb) {
  LOG_DEBUG("[PAST]: mode=%u, skip=%u, timeout=%u", mode, skip, timeout);
  uint8_t cte_type = 7;
  sync_rcvd_cb = syncCb;
  syncRcvdCbRegistered = true;
  if (BleScanningManager::IsInitialized()) {
    BleScanningManager::Get()->SetPeriodicAdvSyncTransferParams(
        addr, mode, skip, timeout, cte_type, true,
        base::Bind(&btm_ble_periodic_syc_transfer_param_cmpl));
  }
}

/*******************************************************************************
 *
 * Function         btm_set_conn_mode_adv_init_addr
 *
 * Description      set initator address type and local address type based on
 *                  adv mode.
 *
 *
 ******************************************************************************/
static uint8_t btm_set_conn_mode_adv_init_addr(
    RawAddress& p_peer_addr_ptr, tBLE_ADDR_TYPE* p_peer_addr_type,
    tBLE_ADDR_TYPE* p_own_addr_type) {
  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;
  uint8_t evt_type;
  tBTM_SEC_DEV_REC* p_dev_rec;

  if (p_cb->connectable_mode == BTM_BLE_NON_CONNECTABLE) {
    if (p_cb->scan_rsp) {
      evt_type = BTM_BLE_DISCOVER_EVT;
    } else {
      evt_type = BTM_BLE_NON_CONNECT_EVT;
    }
  } else {
    evt_type = BTM_BLE_CONNECT_EVT;
  }

  if (evt_type == BTM_BLE_CONNECT_EVT) {
    CHECK(p_peer_addr_type != nullptr);
    const tBLE_BD_ADDR ble_bd_addr = {
        .bda = p_peer_addr_ptr,
        .type = *p_peer_addr_type,
    };
    LOG_DEBUG("Received BLE connect event %s", PRIVATE_ADDRESS(ble_bd_addr));

    evt_type = p_cb->directed_conn;

    if (p_cb->directed_conn == BTM_BLE_CONNECT_DIR_EVT ||
        p_cb->directed_conn == BTM_BLE_CONNECT_LO_DUTY_DIR_EVT) {
      /* for privacy 1.2, convert peer address as static, own address set as ID
       * addr */
      if (btm_cb.ble_ctr_cb.privacy_mode == BTM_PRIVACY_1_2 ||
          btm_cb.ble_ctr_cb.privacy_mode == BTM_PRIVACY_MIXED) {
        /* only do so for bonded device */
        if ((p_dev_rec = btm_find_or_alloc_dev(p_cb->direct_bda.bda)) != NULL &&
            p_dev_rec->ble.in_controller_list & BTM_RESOLVING_LIST_BIT) {
          p_peer_addr_ptr = p_dev_rec->ble.identity_address_with_type.bda;
          *p_peer_addr_type = p_dev_rec->ble.identity_address_with_type.type;
          *p_own_addr_type = BLE_ADDR_RANDOM_ID;
          return evt_type;
        }
        /* otherwise fall though as normal directed adv */
      }
      /* direct adv mode does not have privacy, if privacy is not enabled  */
      *p_peer_addr_type = p_cb->direct_bda.type;
      p_peer_addr_ptr = p_cb->direct_bda.bda;
      return evt_type;
    }
  }

  /* undirect adv mode or non-connectable mode*/
  /* when privacy 1.2 privacy only mode is used, or mixed mode */
  if ((btm_cb.ble_ctr_cb.privacy_mode == BTM_PRIVACY_1_2 &&
       p_cb->afp != AP_SCAN_CONN_ALL) ||
      btm_cb.ble_ctr_cb.privacy_mode == BTM_PRIVACY_MIXED) {
    list_node_t* n =
        list_foreach(btm_cb.sec_dev_rec, is_resolving_list_bit_set, NULL);
    if (n) {
      /* if enhanced privacy is required, set Identity address and matching IRK
       * peer */
      tBTM_SEC_DEV_REC* p_dev_rec =
          static_cast<tBTM_SEC_DEV_REC*>(list_node(n));
      p_peer_addr_ptr = p_dev_rec->ble.identity_address_with_type.bda;
      *p_peer_addr_type = p_dev_rec->ble.identity_address_with_type.type;

      *p_own_addr_type = BLE_ADDR_RANDOM_ID;
    } else {
      /* resolving list is empty, not enabled */
      *p_own_addr_type = BLE_ADDR_RANDOM;
    }
  }
  /* privacy 1.1, or privacy 1.2, general discoverable/connectable mode, disable
     privacy in */
  /* controller fall back to host based privacy */
  else if (btm_cb.ble_ctr_cb.privacy_mode != BTM_PRIVACY_NONE) {
    *p_own_addr_type = BLE_ADDR_RANDOM;
  }

  /* if no privacy,do not set any peer address,*/
  /* local address type go by global privacy setting */
  return evt_type;
}

/**
 * This function is called to set scan parameters. |cb| is called with operation
 * status
 **/
void BTM_BleSetScanParams(uint32_t scan_interval, uint32_t scan_window,
                          tBLE_SCAN_MODE scan_mode,
                          base::Callback<void(uint8_t)> cb) {
  if (!controller_get_interface()->supports_ble()) {
    LOG_INFO("Controller does not support ble");
    return;
  }

  uint32_t max_scan_interval = BTM_BLE_EXT_SCAN_INT_MAX;
  uint32_t max_scan_window = BTM_BLE_EXT_SCAN_WIN_MAX;
  if (btm_cb.cmn_ble_vsc_cb.extended_scan_support == 0) {
    max_scan_interval = BTM_BLE_SCAN_INT_MAX;
    max_scan_window = BTM_BLE_SCAN_WIN_MAX;
  }

  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;
  if (BTM_BLE_ISVALID_PARAM(scan_interval, BTM_BLE_SCAN_INT_MIN,
                            max_scan_interval) &&
      BTM_BLE_ISVALID_PARAM(scan_window, BTM_BLE_SCAN_WIN_MIN,
                            max_scan_window) &&
      (scan_mode == BTM_BLE_SCAN_MODE_ACTI ||
       scan_mode == BTM_BLE_SCAN_MODE_PASS)) {
    p_cb->scan_type = scan_mode;
    p_cb->scan_interval = scan_interval;
    p_cb->scan_window = scan_window;

    cb.Run(BTM_SUCCESS);
  } else {
    cb.Run(BTM_ILLEGAL_VALUE);
    LOG_WARN("Illegal params: scan_interval = %d scan_window = %d",
             scan_interval, scan_window);
  }
}

/*******************************************************************************
 *
 * Function         BTM__BLEReadDiscoverability
 *
 * Description      This function is called to read the current LE
 *                  discoverability mode of the device.
 *
 * Returns          BTM_BLE_NON_DISCOVERABLE ,BTM_BLE_LIMITED_DISCOVERABLE or
 *                     BTM_BLE_GENRAL_DISCOVERABLE
 *
 ******************************************************************************/
uint16_t BTM_BleReadDiscoverability() {
  BTM_TRACE_API("%s", __func__);

  return (btm_cb.ble_ctr_cb.inq_var.discoverable_mode);
}

/*******************************************************************************
 *
 * Function         BTM__BLEReadConnectability
 *
 * Description      This function is called to read the current LE
 *                  connectability mode of the device.
 *
 * Returns          BTM_BLE_NON_CONNECTABLE or BTM_BLE_CONNECTABLE
 *
 ******************************************************************************/
uint16_t BTM_BleReadConnectability() {
  BTM_TRACE_API("%s", __func__);

  return (btm_cb.ble_ctr_cb.inq_var.connectable_mode);
}

/*******************************************************************************
 *
 * Function         btm_ble_select_adv_interval
 *
 * Description      select adv interval based on device mode
 *
 * Returns          void
 *
 ******************************************************************************/
static void btm_ble_select_adv_interval(uint8_t evt_type,
                                        uint16_t* p_adv_int_min,
                                        uint16_t* p_adv_int_max) {
  switch (evt_type) {
    case BTM_BLE_CONNECT_EVT:
    case BTM_BLE_CONNECT_LO_DUTY_DIR_EVT:
      *p_adv_int_min = *p_adv_int_max = BTM_BLE_GAP_ADV_FAST_INT_1;
      break;

    case BTM_BLE_NON_CONNECT_EVT:
    case BTM_BLE_DISCOVER_EVT:
      *p_adv_int_min = *p_adv_int_max = BTM_BLE_GAP_ADV_FAST_INT_2;
      break;

      /* connectable directed event */
    case BTM_BLE_CONNECT_DIR_EVT:
      *p_adv_int_min = BTM_BLE_GAP_ADV_DIR_MIN_INT;
      *p_adv_int_max = BTM_BLE_GAP_ADV_DIR_MAX_INT;
      break;

    default:
      *p_adv_int_min = *p_adv_int_max = BTM_BLE_GAP_ADV_SLOW_INT;
      break;
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_update_dmt_flag_bits
 *
 * Description      Obtain updated adv flag value based on connect and
 *                  discoverability mode. Also, setup DMT support value in the
 *                  flag based on whether the controller supports both LE and
 *                  BR/EDR.
 *
 * Parameters:      flag_value (Input / Output) - flag value
 *                  connect_mode (Input) - Connect mode value
 *                  disc_mode (Input) - discoverability mode
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_update_dmt_flag_bits(uint8_t* adv_flag_value,
                                  const uint16_t connect_mode,
                                  const uint16_t disc_mode) {
  /* BR/EDR non-discoverable , non-connectable */
  if ((disc_mode & BTM_DISCOVERABLE_MASK) == 0 &&
      (connect_mode & BTM_CONNECTABLE_MASK) == 0)
    *adv_flag_value |= BTM_BLE_BREDR_NOT_SPT;
  else
    *adv_flag_value &= ~BTM_BLE_BREDR_NOT_SPT;

  /* if local controller support, mark both controller and host support in flag
   */
  if (controller_get_interface()->supports_simultaneous_le_bredr())
    *adv_flag_value |= (BTM_BLE_DMT_CONTROLLER_SPT | BTM_BLE_DMT_HOST_SPT);
  else
    *adv_flag_value &= ~(BTM_BLE_DMT_CONTROLLER_SPT | BTM_BLE_DMT_HOST_SPT);
}

/*******************************************************************************
 *
 * Function         btm_ble_set_adv_flag
 *
 * Description      Set adv flag in adv data.
 *
 * Parameters:      connect_mode (Input)- Connect mode value
 *                  disc_mode (Input) - discoverability mode
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_set_adv_flag(uint16_t connect_mode, uint16_t disc_mode) {
  uint8_t flag = 0, old_flag = 0;
  tBTM_BLE_LOCAL_ADV_DATA* p_adv_data = &btm_cb.ble_ctr_cb.inq_var.adv_data;

  if (p_adv_data->p_flags != NULL) flag = old_flag = *(p_adv_data->p_flags);

  btm_ble_update_dmt_flag_bits(&flag, connect_mode, disc_mode);

  LOG_INFO("disc_mode %04x", disc_mode);
  /* update discoverable flag */
  if (disc_mode & BTM_BLE_LIMITED_DISCOVERABLE) {
    flag &= ~BTM_BLE_GEN_DISC_FLAG;
    flag |= BTM_BLE_LIMIT_DISC_FLAG;
  } else if (disc_mode & BTM_BLE_GENERAL_DISCOVERABLE) {
    flag |= BTM_BLE_GEN_DISC_FLAG;
    flag &= ~BTM_BLE_LIMIT_DISC_FLAG;
  } else /* remove all discoverable flags */
  {
    flag &= ~(BTM_BLE_LIMIT_DISC_FLAG | BTM_BLE_GEN_DISC_FLAG);
  }

  if (flag != old_flag) {
    btm_ble_update_adv_flag(flag);
  }
}
/*******************************************************************************
 *
 * Function         btm_ble_set_discoverability
 *
 * Description      This function is called to set BLE discoverable mode.
 *
 * Parameters:      combined_mode: discoverability mode.
 *
 * Returns          BTM_SUCCESS is status set successfully; otherwise failure.
 *
 ******************************************************************************/
tBTM_STATUS btm_ble_set_discoverability(uint16_t combined_mode) {
  tBTM_LE_RANDOM_CB* p_addr_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;
  uint16_t mode = (combined_mode & BTM_BLE_DISCOVERABLE_MASK);
  uint8_t new_mode = BTM_BLE_ADV_ENABLE;
  uint8_t evt_type;
  tBTM_STATUS status = BTM_SUCCESS;
  RawAddress address = RawAddress::kEmpty;
  tBLE_ADDR_TYPE init_addr_type = BLE_ADDR_PUBLIC,
                 own_addr_type = p_addr_cb->own_addr_type;
  uint16_t adv_int_min, adv_int_max;

  BTM_TRACE_EVENT("%s mode=0x%0x combined_mode=0x%x", __func__, mode,
                  combined_mode);

  /*** Check mode parameter ***/
  if (mode > BTM_BLE_MAX_DISCOVERABLE) return (BTM_ILLEGAL_VALUE);

  p_cb->discoverable_mode = mode;

  evt_type =
      btm_set_conn_mode_adv_init_addr(address, &init_addr_type, &own_addr_type);

  if (p_cb->connectable_mode == BTM_BLE_NON_CONNECTABLE &&
      mode == BTM_BLE_NON_DISCOVERABLE)
    new_mode = BTM_BLE_ADV_DISABLE;

  btm_ble_select_adv_interval(evt_type, &adv_int_min, &adv_int_max);

  alarm_cancel(p_cb->fast_adv_timer);

  /* update adv params if start advertising */
  BTM_TRACE_EVENT("evt_type=0x%x p-cb->evt_type=0x%x ", evt_type,
                  p_cb->evt_type);

  if (new_mode == BTM_BLE_ADV_ENABLE) {
    btm_ble_set_adv_flag(btm_cb.btm_inq_vars.connectable_mode, combined_mode);

    if (evt_type != p_cb->evt_type || p_cb->adv_addr_type != own_addr_type ||
        !p_cb->fast_adv_on) {
      btm_ble_stop_adv();

      /* update adv params */
      btsnd_hcic_ble_write_adv_params(adv_int_min, adv_int_max, evt_type,
                                      own_addr_type, init_addr_type, address,
                                      p_cb->adv_chnl_map, p_cb->afp);
      p_cb->evt_type = evt_type;
      p_cb->adv_addr_type = own_addr_type;
    }
  }

  if (status == BTM_SUCCESS && p_cb->adv_mode != new_mode) {
    if (new_mode == BTM_BLE_ADV_ENABLE)
      status = btm_ble_start_adv();
    else
      status = btm_ble_stop_adv();
  }

  if (p_cb->adv_mode == BTM_BLE_ADV_ENABLE) {
    p_cb->fast_adv_on = true;
    /* start initial GAP mode adv timer */
    alarm_set_on_mloop(p_cb->fast_adv_timer, BTM_BLE_GAP_FAST_ADV_TIMEOUT_MS,
                       btm_ble_fast_adv_timer_timeout, NULL);
  }

  /* set up stop advertising timer */
  if (status == BTM_SUCCESS && mode == BTM_BLE_LIMITED_DISCOVERABLE) {
    BTM_TRACE_EVENT("start timer for limited disc mode duration=%d ms",
                    BTM_BLE_GAP_LIM_TIMEOUT_MS);
    /* start Tgap(lim_timeout) */
    alarm_set_on_mloop(p_cb->inquiry_timer, BTM_BLE_GAP_LIM_TIMEOUT_MS,
                       btm_ble_inquiry_timer_gap_limited_discovery_timeout,
                       NULL);
  }
  return status;
}

/*******************************************************************************
 *
 * Function         btm_ble_set_connectability
 *
 * Description      This function is called to set BLE connectability mode.
 *
 * Parameters:      combined_mode: connectability mode.
 *
 * Returns          BTM_SUCCESS is status set successfully; otherwise failure.
 *
 ******************************************************************************/
tBTM_STATUS btm_ble_set_connectability(uint16_t combined_mode) {
  tBTM_LE_RANDOM_CB* p_addr_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;
  uint16_t mode = (combined_mode & BTM_BLE_CONNECTABLE_MASK);
  uint8_t new_mode = BTM_BLE_ADV_ENABLE;
  uint8_t evt_type;
  tBTM_STATUS status = BTM_SUCCESS;
  RawAddress address = RawAddress::kEmpty;
  tBLE_ADDR_TYPE peer_addr_type = BLE_ADDR_PUBLIC,
                 own_addr_type = p_addr_cb->own_addr_type;
  uint16_t adv_int_min, adv_int_max;

  BTM_TRACE_EVENT("%s mode=0x%0x combined_mode=0x%x", __func__, mode,
                  combined_mode);

  /*** Check mode parameter ***/
  if (mode > BTM_BLE_MAX_CONNECTABLE) return (BTM_ILLEGAL_VALUE);

  p_cb->connectable_mode = mode;

  evt_type =
      btm_set_conn_mode_adv_init_addr(address, &peer_addr_type, &own_addr_type);

  if (mode == BTM_BLE_NON_CONNECTABLE &&
      p_cb->discoverable_mode == BTM_BLE_NON_DISCOVERABLE)
    new_mode = BTM_BLE_ADV_DISABLE;

  btm_ble_select_adv_interval(evt_type, &adv_int_min, &adv_int_max);

  alarm_cancel(p_cb->fast_adv_timer);
  /* update adv params if needed */
  if (new_mode == BTM_BLE_ADV_ENABLE) {
    btm_ble_set_adv_flag(combined_mode, btm_cb.btm_inq_vars.discoverable_mode);
    if (p_cb->evt_type != evt_type ||
        p_cb->adv_addr_type != p_addr_cb->own_addr_type || !p_cb->fast_adv_on) {
      btm_ble_stop_adv();

      btsnd_hcic_ble_write_adv_params(adv_int_min, adv_int_max, evt_type,
                                      own_addr_type, peer_addr_type, address,
                                      p_cb->adv_chnl_map, p_cb->afp);
      p_cb->evt_type = evt_type;
      p_cb->adv_addr_type = own_addr_type;
    }
  }

  /* update advertising mode */
  if (status == BTM_SUCCESS && new_mode != p_cb->adv_mode) {
    if (new_mode == BTM_BLE_ADV_ENABLE)
      status = btm_ble_start_adv();
    else
      status = btm_ble_stop_adv();
  }

  if (p_cb->adv_mode == BTM_BLE_ADV_ENABLE) {
    p_cb->fast_adv_on = true;
    /* start initial GAP mode adv timer */
    alarm_set_on_mloop(p_cb->fast_adv_timer, BTM_BLE_GAP_FAST_ADV_TIMEOUT_MS,
                       btm_ble_fast_adv_timer_timeout, NULL);
  }
  return status;
}

static void btm_send_hci_scan_enable(uint8_t enable,
                                     uint8_t filter_duplicates) {
  if (controller_get_interface()->supports_ble_extended_advertising()) {
    btsnd_hcic_ble_set_extended_scan_enable(enable, filter_duplicates, 0x0000,
                                            0x0000);
  } else {
    btsnd_hcic_ble_set_scan_enable(enable, filter_duplicates);
  }
}

void btm_send_hci_set_scan_params(uint8_t scan_type, uint16_t scan_int,
                                  uint16_t scan_win,
                                  tBLE_ADDR_TYPE addr_type_own,
                                  uint8_t scan_filter_policy) {
  if (controller_get_interface()->supports_ble_extended_advertising()) {
    scanning_phy_cfg phy_cfg;
    phy_cfg.scan_type = scan_type;
    phy_cfg.scan_int = scan_int;
    phy_cfg.scan_win = scan_win;

    btsnd_hcic_ble_set_extended_scan_params(addr_type_own, scan_filter_policy,
                                            1, &phy_cfg);
  } else {
    btsnd_hcic_ble_set_scan_params(scan_type, scan_int, scan_win, addr_type_own,
                                   scan_filter_policy);
  }
}

/* Scan filter param config event */
static void btm_ble_scan_filt_param_cfg_evt(uint8_t avbl_space,
                                            tBTM_BLE_SCAN_COND_OP action_type,
                                            tBTM_STATUS btm_status) {
  if (btm_status != btm_status_value(BTM_SUCCESS)) {
    BTM_TRACE_ERROR("%s, %d", __func__, btm_status);
  } else {
    BTM_TRACE_DEBUG("%s", __func__);
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_start_inquiry
 *
 * Description      This function is called to start BLE inquiry procedure.
 *                  If the duration is zero, the periodic inquiry mode is
 *                  cancelled.
 *
 * Parameters:      mode - GENERAL or LIMITED inquiry
 *                  p_inq_params - pointer to the BLE inquiry parameter.
 *                  p_results_cb - callback returning pointer to results
 *                                 (tBTM_INQ_RESULTS)
 *                  p_cmpl_cb - callback indicating the end of an inquiry
 *
 *
 *
 * Returns          BTM_CMD_STARTED if successfully started
 *                  BTM_NO_RESOURCES if could not allocate a message buffer
 *                  BTM_BUSY - if an inquiry is already active
 *
 ******************************************************************************/
tBTM_STATUS btm_ble_start_inquiry(uint8_t duration) {
  tBTM_STATUS status = BTM_CMD_STARTED;
  tBTM_BLE_CB* p_ble_cb = &btm_cb.ble_ctr_cb;
  tBTM_INQUIRY_VAR_ST* p_inq = &btm_cb.btm_inq_vars;

  BTM_TRACE_DEBUG("btm_ble_start_inquiry: inq_active = 0x%02x",
                  btm_cb.btm_inq_vars.inq_active);

  /* if selective connection is active, or inquiry is already active, reject it
   */
  if (p_ble_cb->is_ble_inquiry_active()) {
    BTM_TRACE_ERROR("LE Inquiry is active, can not start inquiry");
    return (BTM_BUSY);
  }

  /* Cleanup anything remaining on index 0 */
  BTM_BleAdvFilterParamSetup(BTM_BLE_SCAN_COND_DELETE,
                             static_cast<tBTM_BLE_PF_FILT_INDEX>(0), nullptr,
                             base::Bind(btm_ble_scan_filt_param_cfg_evt));

  auto adv_filt_param = std::make_unique<btgatt_filt_param_setup_t>();
  /* Add an allow-all filter on index 0*/
  adv_filt_param->dely_mode = IMMEDIATE_DELY_MODE;
  adv_filt_param->feat_seln = ALLOW_ALL_FILTER;
  adv_filt_param->filt_logic_type = BTA_DM_BLE_PF_FILT_LOGIC_OR;
  adv_filt_param->list_logic_type = BTA_DM_BLE_PF_LIST_LOGIC_OR;
  adv_filt_param->rssi_low_thres = LOWEST_RSSI_VALUE;
  adv_filt_param->rssi_high_thres = LOWEST_RSSI_VALUE;
  BTM_BleAdvFilterParamSetup(BTM_BLE_SCAN_COND_ADD, static_cast<tBTM_BLE_PF_FILT_INDEX>(0),
                 std::move(adv_filt_param), base::Bind(btm_ble_scan_filt_param_cfg_evt));

  if (!p_ble_cb->is_ble_scan_active()) {
    cache.ClearAll();
    btm_send_hci_set_scan_params(
        BTM_BLE_SCAN_MODE_ACTI, BTM_BLE_LOW_LATENCY_SCAN_INT,
        BTM_BLE_LOW_LATENCY_SCAN_WIN,
        btm_cb.ble_ctr_cb.addr_mgnt_cb.own_addr_type, SP_ADV_ALL);
    p_ble_cb->inq_var.scan_type = BTM_BLE_SCAN_MODE_ACTI;
    btm_ble_start_scan();
  } else if ((p_ble_cb->inq_var.scan_interval !=
              BTM_BLE_LOW_LATENCY_SCAN_INT) ||
             (p_ble_cb->inq_var.scan_window != BTM_BLE_LOW_LATENCY_SCAN_WIN)) {
    BTM_TRACE_DEBUG("%s, restart LE scan with low latency scan params",
                    __func__);
    btm_send_hci_scan_enable(BTM_BLE_SCAN_DISABLE, BTM_BLE_DUPLICATE_ENABLE);
    btm_send_hci_set_scan_params(
        BTM_BLE_SCAN_MODE_ACTI, BTM_BLE_LOW_LATENCY_SCAN_INT,
        BTM_BLE_LOW_LATENCY_SCAN_WIN,
        btm_cb.ble_ctr_cb.addr_mgnt_cb.own_addr_type, SP_ADV_ALL);
    btm_send_hci_scan_enable(BTM_BLE_SCAN_ENABLE, BTM_BLE_DUPLICATE_DISABLE);
  }

  if (status == BTM_CMD_STARTED) {
    p_inq->inq_active |= BTM_BLE_GENERAL_INQUIRY;
    p_ble_cb->set_ble_inquiry_active();

    BTM_TRACE_DEBUG("btm_ble_start_inquiry inq_active = 0x%02x",
                    p_inq->inq_active);

    if (duration != 0) {
      /* start inquiry timer */
      uint64_t duration_ms = duration * 1000;
      alarm_set_on_mloop(p_ble_cb->inq_var.inquiry_timer, duration_ms,
                         btm_ble_inquiry_timer_timeout, NULL);
    }
  }

  return status;
}

/*******************************************************************************
 *
 * Function         btm_ble_read_remote_name_cmpl
 *
 * Description      This function is called when BLE remote name is received.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_read_remote_name_cmpl(bool status, const RawAddress& bda,
                                   uint16_t length, char* p_name) {
  tHCI_STATUS hci_status = HCI_SUCCESS;
  BD_NAME bd_name;

  memset(bd_name, 0, (BD_NAME_LEN + 1));
  if (length > BD_NAME_LEN) {
    length = BD_NAME_LEN;
  }
  memcpy((uint8_t*)bd_name, p_name, length);

  if ((!status) || (length == 0)) {
    hci_status = HCI_ERR_HOST_TIMEOUT;
  }

  btm_process_remote_name(&bda, bd_name, length + 1, hci_status);
  btm_sec_rmt_name_request_complete(&bda, (const uint8_t*)p_name, hci_status);
}

/*******************************************************************************
 *
 * Function         btm_ble_read_remote_name
 *
 * Description      This function read remote LE device name using GATT read
 *                  procedure.
 *
 * Parameters:       None.
 *
 * Returns          void
 *
 ******************************************************************************/
tBTM_STATUS btm_ble_read_remote_name(const RawAddress& remote_bda,
                                     tBTM_CMPL_CB* p_cb) {
  tBTM_INQUIRY_VAR_ST* p_inq = &btm_cb.btm_inq_vars;

  if (!controller_get_interface()->supports_ble()) return BTM_ERR_PROCESSING;

  tINQ_DB_ENT* p_i = btm_inq_db_find(remote_bda);
  if (p_i && !ble_evt_type_is_connectable(p_i->inq_info.results.ble_evt_type)) {
    BTM_TRACE_DEBUG("name request to non-connectable device failed.");
    return BTM_ERR_PROCESSING;
  }

  /* read remote device name using GATT procedure */
  if (p_inq->remname_active) return BTM_BUSY;

  if (!GAP_BleReadPeerDevName(remote_bda, btm_ble_read_remote_name_cmpl))
    return BTM_BUSY;

  p_inq->p_remname_cmpl_cb = p_cb;
  p_inq->remname_active = true;
  p_inq->remname_bda = remote_bda;

  alarm_set_on_mloop(p_inq->remote_name_timer, BTM_EXT_BLE_RMT_NAME_TIMEOUT_MS,
                     btm_inq_remote_name_timer_timeout, NULL);

  return BTM_CMD_STARTED;
}

/*******************************************************************************
 *
 * Function         btm_ble_cancel_remote_name
 *
 * Description      This function cancel read remote LE device name.
 *
 * Parameters:       None.
 *
 * Returns          void
 *
 ******************************************************************************/
bool btm_ble_cancel_remote_name(const RawAddress& remote_bda) {
  tBTM_INQUIRY_VAR_ST* p_inq = &btm_cb.btm_inq_vars;
  bool status;

  status = GAP_BleCancelReadPeerDevName(remote_bda);

  p_inq->remname_active = false;
  p_inq->remname_bda = RawAddress::kEmpty;
  alarm_cancel(p_inq->remote_name_timer);

  return status;
}

/*******************************************************************************
 *
 * Function         btm_ble_update_adv_flag
 *
 * Description      This function update the limited discoverable flag in the
 *                  adv data.
 *
 * Parameters:       None.
 *
 * Returns          void
 *
 ******************************************************************************/
static void btm_ble_update_adv_flag(uint8_t flag) {
  tBTM_BLE_LOCAL_ADV_DATA* p_adv_data = &btm_cb.ble_ctr_cb.inq_var.adv_data;
  uint8_t* p;

  BTM_TRACE_DEBUG("btm_ble_update_adv_flag new=0x%x", flag);

  if (p_adv_data->p_flags != NULL) {
    BTM_TRACE_DEBUG("btm_ble_update_adv_flag old=0x%x", *p_adv_data->p_flags);
    *p_adv_data->p_flags = flag;
  } else /* no FLAGS in ADV data*/
  {
    p = (p_adv_data->p_pad == NULL) ? p_adv_data->ad_data : p_adv_data->p_pad;
    /* need 3 bytes space to stuff in the flags, if not */
    /* erase all written data, just for flags */
    if ((BTM_BLE_AD_DATA_LEN - (p - p_adv_data->ad_data)) < 3) {
      p = p_adv_data->p_pad = p_adv_data->ad_data;
      memset(p_adv_data->ad_data, 0, BTM_BLE_AD_DATA_LEN);
    }

    *p++ = 2;
    *p++ = BTM_BLE_AD_TYPE_FLAG;
    p_adv_data->p_flags = p;
    *p++ = flag;
    p_adv_data->p_pad = p;
  }

  btsnd_hcic_ble_set_adv_data(
      (uint8_t)(p_adv_data->p_pad - p_adv_data->ad_data), p_adv_data->ad_data);
  p_adv_data->data_mask |= BTM_BLE_AD_BIT_FLAGS;
}

/**
 * Check ADV flag to make sure device is discoverable and match the search
 * condition
 */
static uint8_t btm_ble_is_discoverable(const RawAddress& bda,
                                       std::vector<uint8_t> const& adv_data) {
  uint8_t scan_state = BTM_BLE_NOT_SCANNING;

  /* for observer, always "discoverable */
  if (btm_cb.ble_ctr_cb.is_ble_observe_active())
    scan_state |= BTM_BLE_OBS_RESULT;

  if (!adv_data.empty()) {
    uint8_t flag = 0;
    uint8_t data_len;
    const uint8_t* p_flag = AdvertiseDataParser::GetFieldByType(
        adv_data, BTM_BLE_AD_TYPE_FLAG, &data_len);
    if (p_flag != NULL && data_len != 0) {
      flag = *p_flag;

      if ((btm_cb.btm_inq_vars.inq_active & BTM_BLE_GENERAL_INQUIRY) &&
          (flag & (BTM_BLE_LIMIT_DISC_FLAG | BTM_BLE_GEN_DISC_FLAG)) != 0) {
        scan_state |= BTM_BLE_INQ_RESULT;
      }
    }
  }
  return scan_state;
}

static void btm_ble_appearance_to_cod(uint16_t appearance, uint8_t* dev_class) {
  dev_class[0] = 0;

  switch (appearance) {
    case BTM_BLE_APPEARANCE_GENERIC_PHONE:
      dev_class[1] = BTM_COD_MAJOR_PHONE;
      dev_class[2] = BTM_COD_MINOR_UNCLASSIFIED;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_COMPUTER:
      dev_class[1] = BTM_COD_MAJOR_COMPUTER;
      dev_class[2] = BTM_COD_MINOR_UNCLASSIFIED;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_REMOTE:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_REMOTE_CONTROL;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_THERMOMETER:
    case BTM_BLE_APPEARANCE_THERMOMETER_EAR:
      dev_class[1] = BTM_COD_MAJOR_HEALTH;
      dev_class[2] = BTM_COD_MINOR_THERMOMETER;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_HEART_RATE:
    case BTM_BLE_APPEARANCE_HEART_RATE_BELT:
      dev_class[1] = BTM_COD_MAJOR_HEALTH;
      dev_class[2] = BTM_COD_MINOR_HEART_PULSE_MONITOR;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_BLOOD_PRESSURE:
    case BTM_BLE_APPEARANCE_BLOOD_PRESSURE_ARM:
    case BTM_BLE_APPEARANCE_BLOOD_PRESSURE_WRIST:
      dev_class[1] = BTM_COD_MAJOR_HEALTH;
      dev_class[2] = BTM_COD_MINOR_BLOOD_MONITOR;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_PULSE_OXIMETER:
    case BTM_BLE_APPEARANCE_PULSE_OXIMETER_FINGERTIP:
    case BTM_BLE_APPEARANCE_PULSE_OXIMETER_WRIST:
      dev_class[1] = BTM_COD_MAJOR_HEALTH;
      dev_class[2] = BTM_COD_MINOR_PULSE_OXIMETER;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_GLUCOSE:
      dev_class[1] = BTM_COD_MAJOR_HEALTH;
      dev_class[2] = BTM_COD_MINOR_GLUCOSE_METER;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_WEIGHT:
      dev_class[1] = BTM_COD_MAJOR_HEALTH;
      dev_class[2] = BTM_COD_MINOR_WEIGHING_SCALE;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_WALKING:
    case BTM_BLE_APPEARANCE_WALKING_IN_SHOE:
    case BTM_BLE_APPEARANCE_WALKING_ON_SHOE:
    case BTM_BLE_APPEARANCE_WALKING_ON_HIP:
      dev_class[1] = BTM_COD_MAJOR_HEALTH;
      dev_class[2] = BTM_COD_MINOR_STEP_COUNTER;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_WATCH:
    case BTM_BLE_APPEARANCE_SPORTS_WATCH:
      dev_class[1] = BTM_COD_MAJOR_WEARABLE;
      dev_class[2] = BTM_COD_MINOR_WRIST_WATCH;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_EYEGLASSES:
      dev_class[1] = BTM_COD_MAJOR_WEARABLE;
      dev_class[2] = BTM_COD_MINOR_GLASSES;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_DISPLAY:
      dev_class[1] = BTM_COD_MAJOR_IMAGING;
      dev_class[2] = BTM_COD_MINOR_DISPLAY;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_MEDIA_PLAYER:
      dev_class[1] = BTM_COD_MAJOR_AUDIO;
      dev_class[2] = BTM_COD_MINOR_UNCLASSIFIED;
      break;
    case BTM_BLE_APPEARANCE_WEARABLE_AUDIO_DEVICE_EARBUD:
      dev_class[1] = BTM_COD_MAJOR_AUDIO;
      dev_class[2] = BTM_COD_MINOR_WEARABLE_HEADSET;
      break;
    case BTM_BLE_APPEARANCE_GENERIC_BARCODE_SCANNER:
    case BTM_BLE_APPEARANCE_HID_BARCODE_SCANNER:
    case BTM_BLE_APPEARANCE_GENERIC_HID:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_UNCLASSIFIED;
      break;
    case BTM_BLE_APPEARANCE_HID_KEYBOARD:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_KEYBOARD;
      break;
    case BTM_BLE_APPEARANCE_HID_MOUSE:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_POINTING;
      break;
    case BTM_BLE_APPEARANCE_HID_JOYSTICK:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_JOYSTICK;
      break;
    case BTM_BLE_APPEARANCE_HID_GAMEPAD:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_GAMEPAD;
      break;
    case BTM_BLE_APPEARANCE_HID_DIGITIZER_TABLET:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_DIGITIZING_TABLET;
      break;
    case BTM_BLE_APPEARANCE_HID_CARD_READER:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_CARD_READER;
      break;
    case BTM_BLE_APPEARANCE_HID_DIGITAL_PEN:
      dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
      dev_class[2] = BTM_COD_MINOR_DIGITAL_PAN;
      break;
    case BTM_BLE_APPEARANCE_UKNOWN:
    case BTM_BLE_APPEARANCE_GENERIC_CLOCK:
    case BTM_BLE_APPEARANCE_GENERIC_TAG:
    case BTM_BLE_APPEARANCE_GENERIC_KEYRING:
    case BTM_BLE_APPEARANCE_GENERIC_CYCLING:
    case BTM_BLE_APPEARANCE_CYCLING_COMPUTER:
    case BTM_BLE_APPEARANCE_CYCLING_SPEED:
    case BTM_BLE_APPEARANCE_CYCLING_CADENCE:
    case BTM_BLE_APPEARANCE_CYCLING_POWER:
    case BTM_BLE_APPEARANCE_CYCLING_SPEED_CADENCE:
    case BTM_BLE_APPEARANCE_GENERIC_OUTDOOR_SPORTS:
    case BTM_BLE_APPEARANCE_OUTDOOR_SPORTS_LOCATION:
    case BTM_BLE_APPEARANCE_OUTDOOR_SPORTS_LOCATION_AND_NAV:
    case BTM_BLE_APPEARANCE_OUTDOOR_SPORTS_LOCATION_POD:
    case BTM_BLE_APPEARANCE_OUTDOOR_SPORTS_LOCATION_POD_AND_NAV:
    default:
      dev_class[1] = BTM_COD_MAJOR_UNCLASSIFIED;
      dev_class[2] = BTM_COD_MINOR_UNCLASSIFIED;
  };
}

/**
 * Update adv packet information into inquiry result.
 */
void btm_ble_update_inq_result(tINQ_DB_ENT* p_i, uint8_t addr_type,
                               const RawAddress& bda, uint16_t evt_type,
                               uint8_t primary_phy, uint8_t secondary_phy,
                               uint8_t advertising_sid, int8_t tx_power,
                               int8_t rssi, uint16_t periodic_adv_int,
                               std::vector<uint8_t> const& data) {
  tBTM_INQ_RESULTS* p_cur = &p_i->inq_info.results;
  uint8_t len;
  tBTM_INQUIRY_VAR_ST* p_inq = &btm_cb.btm_inq_vars;

  /* Save the info */
  p_cur->inq_result_type |= BTM_INQ_RESULT_BLE;
  p_cur->ble_addr_type = static_cast<tBLE_ADDR_TYPE>(addr_type);
  p_cur->rssi = rssi;
  p_cur->ble_primary_phy = primary_phy;
  p_cur->ble_secondary_phy = secondary_phy;
  p_cur->ble_advertising_sid = advertising_sid;
  p_cur->ble_tx_power = tx_power;
  p_cur->ble_periodic_adv_int = periodic_adv_int;

  if (btm_cb.ble_ctr_cb.inq_var.scan_type == BTM_BLE_SCAN_MODE_ACTI &&
      ble_evt_type_is_scannable(evt_type) &&
      !ble_evt_type_is_scan_resp(evt_type)) {
    p_i->scan_rsp = false;
  } else
    p_i->scan_rsp = true;

  if (p_i->inq_count != p_inq->inq_counter)
    p_cur->device_type = BT_DEVICE_TYPE_BLE;
  else
    p_cur->device_type |= BT_DEVICE_TYPE_BLE;

  if (evt_type != BTM_BLE_SCAN_RSP_EVT) p_cur->ble_evt_type = evt_type;

  p_i->inq_count = p_inq->inq_counter; /* Mark entry for current inquiry */

  bool has_advertising_flags = false;
  if (!data.empty()) {
    const uint8_t* p_flag =
        AdvertiseDataParser::GetFieldByType(data, BTM_BLE_AD_TYPE_FLAG, &len);
    if (p_flag != NULL && len != 0) {
      has_advertising_flags = true;
      p_cur->flag = *p_flag;
    }
  }

  if (!data.empty()) {
    /* Check to see the BLE device has the Appearance UUID in the advertising
     * data.  If it does
     * then try to convert the appearance value to a class of device value
     * Bluedroid can use.
     * Otherwise fall back to trying to infer if it is a HID device based on the
     * service class.
     */
    const uint8_t* p_uuid16 = AdvertiseDataParser::GetFieldByType(
        data, BTM_BLE_AD_TYPE_APPEARANCE, &len);
    if (p_uuid16 && len == 2) {
      btm_ble_appearance_to_cod((uint16_t)p_uuid16[0] | (p_uuid16[1] << 8),
                                p_cur->dev_class);
    } else {
      p_uuid16 = AdvertiseDataParser::GetFieldByType(
          data, BTM_BLE_AD_TYPE_16SRV_CMPL, &len);
      if (p_uuid16 != NULL) {
        uint8_t i;
        for (i = 0; i + 2 <= len; i = i + 2) {
          /* if this BLE device support HID over LE, set HID Major in class of
           * device */
          if ((p_uuid16[i] | (p_uuid16[i + 1] << 8)) == UUID_SERVCLASS_LE_HID) {
            p_cur->dev_class[0] = 0;
            p_cur->dev_class[1] = BTM_COD_MAJOR_PERIPHERAL;
            p_cur->dev_class[2] = 0;
            break;
          }
        }
      }
    }
  }

  // Non-connectable packets may omit flags entirely, in which case nothing
  // should be assumed about their values (CSSv10, 1.3.1). Thus, do not
  // interpret the device type unless this packet has the flags set or is
  // connectable.
  bool should_process_flags =
      has_advertising_flags || ble_evt_type_is_connectable(evt_type);
  if (should_process_flags && (p_cur->flag & BTM_BLE_BREDR_NOT_SPT) == 0 &&
      !ble_evt_type_is_directed(evt_type)) {
    if (p_cur->ble_addr_type != BLE_ADDR_RANDOM) {
      LOG_VERBOSE("NOT_BR_EDR support bit not set, treat device as DUMO");
      p_cur->device_type |= BT_DEVICE_TYPE_DUMO;
    } else {
      LOG_VERBOSE("Random address, treat device as LE only");
    }
  } else {
    LOG_VERBOSE("NOT_BR/EDR support bit set, treat device as LE only");
  }
}

/*******************************************************************************
 *
 * Function         btm_clear_all_pending_le_entry
 *
 * Description      This function is called to clear all LE pending entry in
 *                  inquiry database.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_clear_all_pending_le_entry(void) {
  uint16_t xx;
  tINQ_DB_ENT* p_ent = btm_cb.btm_inq_vars.inq_db;

  for (xx = 0; xx < BTM_INQ_DB_SIZE; xx++, p_ent++) {
    /* mark all pending LE entry as unused if an LE only device has scan
     * response outstanding */
    if ((p_ent->in_use) &&
        (p_ent->inq_info.results.device_type == BT_DEVICE_TYPE_BLE) &&
        !p_ent->scan_rsp)
      p_ent->in_use = false;
  }
}

void btm_ble_process_adv_addr(RawAddress& bda, tBLE_ADDR_TYPE* addr_type) {
  /* map address to security record */
  bool match = btm_identity_addr_to_random_pseudo(&bda, addr_type, false);

  VLOG(1) << __func__ << ": bda=" << bda;
  /* always do RRA resolution on host */
  if (!match && BTM_BLE_IS_RESOLVE_BDA(bda)) {
    tBTM_SEC_DEV_REC* match_rec = btm_ble_resolve_random_addr(bda);
    if (match_rec) {
      match_rec->ble.active_addr_type = tBTM_SEC_BLE::BTM_BLE_ADDR_RRA;
      match_rec->ble.cur_rand_addr = bda;

      if (btm_ble_init_pseudo_addr(match_rec, bda)) {
        bda = match_rec->bd_addr;
      } else {
        // Assign the original address to be the current report address
        bda = match_rec->ble.pseudo_addr;
        *addr_type = match_rec->ble.AddressType();
      }
    }
  }
}

/**
 * This function is called when extended advertising report event is received .
 * It updates the inquiry database. If the inquiry database is full, the oldest
 * entry is discarded.
 */
void btm_ble_process_ext_adv_pkt(uint8_t data_len, const uint8_t* data) {
  RawAddress bda, direct_address;
  const uint8_t* p = data;
  uint8_t addr_type, num_reports, pkt_data_len, primary_phy, secondary_phy,
      advertising_sid;
  int8_t rssi, tx_power;
  uint16_t event_type, periodic_adv_int, direct_address_type;

  /* Only process the results if the inquiry is still active */
  if (!btm_cb.ble_ctr_cb.is_ble_scan_active()) return;

  /* Extract the number of reports in this event. */
  STREAM_TO_UINT8(num_reports, p);

  constexpr int extended_report_header_size = 24;
  while (num_reports--) {
    if (p + extended_report_header_size > data + data_len) {
      // TODO(jpawlowski): we should crash the stack here
      BTM_TRACE_ERROR(
          "Malformed LE Extended Advertising Report Event from controller - "
          "can't loop the data");
      return;
    }

    /* Extract inquiry results */
    STREAM_TO_UINT16(event_type, p);
    STREAM_TO_UINT8(addr_type, p);
    STREAM_TO_BDADDR(bda, p);
    STREAM_TO_UINT8(primary_phy, p);
    STREAM_TO_UINT8(secondary_phy, p);
    STREAM_TO_UINT8(advertising_sid, p);
    STREAM_TO_INT8(tx_power, p);
    STREAM_TO_INT8(rssi, p);
    STREAM_TO_UINT16(periodic_adv_int, p);
    STREAM_TO_UINT8(direct_address_type, p);
    STREAM_TO_BDADDR(direct_address, p);
    STREAM_TO_UINT8(pkt_data_len, p);

    const uint8_t* pkt_data = p;
    p += pkt_data_len; /* Advance to the the next packet*/
    if (p > data + data_len) {
      LOG(ERROR) << "Invalid pkt_data_len: " << +pkt_data_len;
      return;
    }

    if (rssi >= 21 && rssi <= 126) {
      BTM_TRACE_ERROR("%s: bad rssi value in advertising report: %d", __func__,
                      rssi);
    }

    // Store this to pass up the callback chain to GattService#onScanResult for
    // the check in ScanFilter#matches
    RawAddress original_bda = bda;

    if (addr_type != BLE_ADDR_ANONYMOUS) {
      btm_ble_process_adv_addr(bda, &addr_type);
    }

    btm_ble_process_adv_pkt_cont(
        event_type, addr_type, bda, primary_phy, secondary_phy, advertising_sid,
        tx_power, rssi, periodic_adv_int, pkt_data_len, pkt_data, original_bda);
  }
}

/**
 * This function is called when advertising report event is received. It updates
 * the inquiry database. If the inquiry database is full, the oldest entry is
 * discarded.
 */
void btm_ble_process_adv_pkt(uint8_t data_len, const uint8_t* data) {
  RawAddress bda;
  const uint8_t* p = data;
  uint8_t legacy_evt_type, addr_type, num_reports, pkt_data_len;
  int8_t rssi;

  /* Only process the results if the inquiry is still active */
  if (!btm_cb.ble_ctr_cb.is_ble_scan_active()) return;

  /* Extract the number of reports in this event. */
  STREAM_TO_UINT8(num_reports, p);

  constexpr int report_header_size = 10;
  while (num_reports--) {
    if (p + report_header_size > data + data_len) {
      // TODO(jpawlowski): we should crash the stack here
      BTM_TRACE_ERROR("Malformed LE Advertising Report Event from controller");
      return;
    }

    /* Extract inquiry results */
    STREAM_TO_UINT8(legacy_evt_type, p);
    STREAM_TO_UINT8(addr_type, p);
    STREAM_TO_BDADDR(bda, p);
    STREAM_TO_UINT8(pkt_data_len, p);

    const uint8_t* pkt_data = p;
    p += pkt_data_len; /* Advance to the the rssi byte */
    if (p > data + data_len - sizeof(rssi)) {
      LOG(ERROR) << "Invalid pkt_data_len: " << +pkt_data_len;
      return;
    }

    STREAM_TO_INT8(rssi, p);

    if (rssi >= 21 && rssi <= 126) {
      BTM_TRACE_ERROR("%s: bad rssi value in advertising report: ", __func__,
                      pkt_data_len, rssi);
    }

    // Pass up the address to GattService#onScanResult to use in
    // ScanFilter#matches
    RawAddress original_bda = bda;

    btm_ble_process_adv_addr(bda, &addr_type);

    uint16_t event_type;
    event_type = 1 << BLE_EVT_LEGACY_BIT;
    if (legacy_evt_type == BTM_BLE_ADV_IND_EVT) {
      event_type |=
          (1 << BLE_EVT_CONNECTABLE_BIT) | (1 << BLE_EVT_SCANNABLE_BIT);
    } else if (legacy_evt_type == BTM_BLE_ADV_DIRECT_IND_EVT) {
      event_type |=
          (1 << BLE_EVT_CONNECTABLE_BIT) | (1 << BLE_EVT_DIRECTED_BIT);
    } else if (legacy_evt_type == BTM_BLE_ADV_SCAN_IND_EVT) {
      event_type |= (1 << BLE_EVT_SCANNABLE_BIT);
    } else if (legacy_evt_type == BTM_BLE_ADV_NONCONN_IND_EVT) {
      event_type = (1 << BLE_EVT_LEGACY_BIT);              // 0x0010;
    } else if (legacy_evt_type == BTM_BLE_SCAN_RSP_EVT) {  // SCAN_RSP;
      // We can't distinguish between "SCAN_RSP to an ADV_IND", and "SCAN_RSP to
      // an ADV_SCAN_IND", so always return "SCAN_RSP to an ADV_IND"
      event_type |= (1 << BLE_EVT_CONNECTABLE_BIT) |
                    (1 << BLE_EVT_SCANNABLE_BIT) |
                    (1 << BLE_EVT_SCAN_RESPONSE_BIT);
    } else {
      BTM_TRACE_ERROR(
          "Malformed LE Advertising Report Event - unsupported "
          "legacy_event_type 0x%02x",
          legacy_evt_type);
      return;
    }

    btm_ble_process_adv_pkt_cont(
        event_type, addr_type, bda, PHY_LE_1M, PHY_LE_NO_PACKET, NO_ADI_PRESENT,
        TX_POWER_NOT_PRESENT, rssi, 0x00 /* no periodic adv */, pkt_data_len,
        pkt_data, original_bda);
  }
}

/**
 * This function is called after random address resolution is done, and proceed
 * to process adv packet.
 */
void btm_ble_process_adv_pkt_cont(uint16_t evt_type, tBLE_ADDR_TYPE addr_type,
                                  const RawAddress& bda, uint8_t primary_phy,
                                  uint8_t secondary_phy,
                                  uint8_t advertising_sid, int8_t tx_power,
                                  int8_t rssi, uint16_t periodic_adv_int,
                                  uint8_t data_len, const uint8_t* data,
                                  const RawAddress& original_bda) {
  tBTM_INQUIRY_VAR_ST* p_inq = &btm_cb.btm_inq_vars;
  bool update = true;

  std::vector<uint8_t> tmp;
  if (data_len != 0) tmp.insert(tmp.begin(), data, data + data_len);

  bool is_scannable = ble_evt_type_is_scannable(evt_type);
  bool is_scan_resp = ble_evt_type_is_scan_resp(evt_type);
  bool is_legacy = ble_evt_type_is_legacy(evt_type);

  // We might receive a legacy scan response without receving a ADV_IND
  // or ADV_SCAN_IND before. Only parsing the scan response data which
  // has no ad flag, the device will be set to DUMO mode. The createbond
  // procedure will use the wrong device mode.
  // In such case no necessary to report scan response
  if (is_legacy && is_scan_resp && !cache.Exist(addr_type, bda)) return;

  bool is_start = is_legacy && is_scannable && !is_scan_resp;

  if (is_legacy) AdvertiseDataParser::RemoveTrailingZeros(tmp);

  // We might have send scan request to this device before, but didn't get the
  // response. In such case make sure data is put at start, not appended to
  // already existing data.
  std::vector<uint8_t> const& adv_data =
      is_start ? cache.Set(addr_type, bda, std::move(tmp))
               : cache.Append(addr_type, bda, std::move(tmp));

  bool data_complete = (ble_evt_type_data_status(evt_type) != 0x01);

  if (!data_complete) {
    // If we didn't receive whole adv data yet, don't report the device.
    DVLOG(1) << "Data not complete yet, waiting for more " << bda;
    return;
  }

  bool is_active_scan =
      btm_cb.ble_ctr_cb.inq_var.scan_type == BTM_BLE_SCAN_MODE_ACTI;
  if (is_active_scan && is_scannable && !is_scan_resp) {
    // If we didn't receive scan response yet, don't report the device.
    DVLOG(1) << " Waiting for scan response " << bda;
    return;
  }

  if (!AdvertiseDataParser::IsValid(adv_data)) {
    DVLOG(1) << __func__ << "Dropping bad advertisement packet: "
             << base::HexEncode(adv_data.data(), adv_data.size());
    cache.Clear(addr_type, bda);
    return;
  }

  bool include_rsi = false;
  uint8_t len;
  if (AdvertiseDataParser::GetFieldByType(adv_data, BTM_BLE_AD_TYPE_RSI,
                                          &len)) {
    include_rsi = true;
  }

  tINQ_DB_ENT* p_i = btm_inq_db_find(bda);

  /* Check if this address has already been processed for this inquiry */
  if (btm_inq_find_bdaddr(bda)) {
    /* never been report as an LE device */
    if (p_i && (!(p_i->inq_info.results.device_type & BT_DEVICE_TYPE_BLE) ||
                /* scan response to be updated */
                (!p_i->scan_rsp) ||
                (!p_i->inq_info.results.include_rsi && include_rsi))) {
      update = true;
    } else if (btm_cb.ble_ctr_cb.is_ble_observe_active()) {
      update = false;
    } else {
      /* if yes, skip it */
      cache.Clear(addr_type, bda);
      return; /* assumption: one result per event */
    }
  }
  /* If existing entry, use that, else get  a new one (possibly reusing the
   * oldest) */
  if (p_i == NULL) {
    p_i = btm_inq_db_new(bda);
    if (p_i != NULL) {
      p_inq->inq_cmpl_info.num_resp++;
      p_i->time_of_resp = bluetooth::common::time_get_os_boottime_ms();
    } else
      return;
  } else if (p_i->inq_count !=
             p_inq->inq_counter) /* first time seen in this inquiry */
  {
    p_i->time_of_resp = bluetooth::common::time_get_os_boottime_ms();
    p_inq->inq_cmpl_info.num_resp++;
  }

  /* update the LE device information in inquiry database */
  btm_ble_update_inq_result(p_i, addr_type, bda, evt_type, primary_phy,
                            secondary_phy, advertising_sid, tx_power, rssi,
                            periodic_adv_int, adv_data);

  if (include_rsi) {
    (&p_i->inq_info.results)->include_rsi = true;
  }

  tBTM_INQ_RESULTS_CB* p_opportunistic_obs_results_cb =
      btm_cb.ble_ctr_cb.p_opportunistic_obs_results_cb;
  if (p_opportunistic_obs_results_cb) {
    (p_opportunistic_obs_results_cb)((tBTM_INQ_RESULTS*)&p_i->inq_info.results,
                                     const_cast<uint8_t*>(adv_data.data()),
                                     adv_data.size());
  }

  uint8_t result = btm_ble_is_discoverable(bda, adv_data);
  if (result == 0) {
    // Device no longer discoverable so discard outstanding advertising packet
    cache.Clear(addr_type, bda);
    return;
  }

  if (!update) result &= ~BTM_BLE_INQ_RESULT;

  tBTM_INQ_RESULTS_CB* p_inq_results_cb = p_inq->p_inq_results_cb;
  if (p_inq_results_cb && (result & BTM_BLE_INQ_RESULT)) {
    (p_inq_results_cb)((tBTM_INQ_RESULTS*)&p_i->inq_info.results,
                       const_cast<uint8_t*>(adv_data.data()), adv_data.size());
  }

  // Pass address up to GattService#onScanResult
  p_i->inq_info.results.original_bda = original_bda;

  tBTM_INQ_RESULTS_CB* p_obs_results_cb = btm_cb.ble_ctr_cb.p_obs_results_cb;
  if (p_obs_results_cb && (result & BTM_BLE_OBS_RESULT)) {
    (p_obs_results_cb)((tBTM_INQ_RESULTS*)&p_i->inq_info.results,
                       const_cast<uint8_t*>(adv_data.data()), adv_data.size());
  }

  cache.Clear(addr_type, bda);
}

/**
 * This function copy from btm_ble_process_adv_pkt_cont to process adv packet
 * from gd scanning module to handle inquiry result callback.
 */
void btm_ble_process_adv_pkt_cont_for_inquiry(
    uint16_t evt_type, tBLE_ADDR_TYPE addr_type, const RawAddress& bda,
    uint8_t primary_phy, uint8_t secondary_phy, uint8_t advertising_sid,
    int8_t tx_power, int8_t rssi, uint16_t periodic_adv_int,
    std::vector<uint8_t> advertising_data) {
  tBTM_INQUIRY_VAR_ST* p_inq = &btm_cb.btm_inq_vars;
  bool update = true;

  bool include_rsi = false;
  uint8_t len;
  if (AdvertiseDataParser::GetFieldByType(advertising_data, BTM_BLE_AD_TYPE_RSI,
                                          &len)) {
    include_rsi = true;
  }

  tINQ_DB_ENT* p_i = btm_inq_db_find(bda);

  /* Check if this address has already been processed for this inquiry */
  if (btm_inq_find_bdaddr(bda)) {
    /* never been report as an LE device */
    if (p_i && (!(p_i->inq_info.results.device_type & BT_DEVICE_TYPE_BLE) ||
                /* scan response to be updated */
                (!p_i->scan_rsp) ||
                (!p_i->inq_info.results.include_rsi && include_rsi))) {
      update = true;
    } else if (btm_cb.ble_ctr_cb.is_ble_observe_active()) {
      update = false;
    } else {
      /* if yes, skip it */
      return; /* assumption: one result per event */
    }
  }

  /* If existing entry, use that, else get  a new one (possibly reusing the
   * oldest) */
  if (p_i == NULL) {
    p_i = btm_inq_db_new(bda);
    if (p_i != NULL) {
      p_inq->inq_cmpl_info.num_resp++;
      p_i->time_of_resp = bluetooth::common::time_get_os_boottime_ms();
    } else
      return;
  } else if (p_i->inq_count !=
             p_inq->inq_counter) /* first time seen in this inquiry */
  {
    p_i->time_of_resp = bluetooth::common::time_get_os_boottime_ms();
    p_inq->inq_cmpl_info.num_resp++;
  }

  /* update the LE device information in inquiry database */
  btm_ble_update_inq_result(p_i, addr_type, bda, evt_type, primary_phy,
                            secondary_phy, advertising_sid, tx_power, rssi,
                            periodic_adv_int, advertising_data);

  if (include_rsi) {
    (&p_i->inq_info.results)->include_rsi = true;
  }

  tBTM_INQ_RESULTS_CB* p_opportunistic_obs_results_cb =
      btm_cb.ble_ctr_cb.p_opportunistic_obs_results_cb;
  if (p_opportunistic_obs_results_cb) {
    (p_opportunistic_obs_results_cb)(
        (tBTM_INQ_RESULTS*)&p_i->inq_info.results,
        const_cast<uint8_t*>(advertising_data.data()), advertising_data.size());
  }

  uint8_t result = btm_ble_is_discoverable(bda, advertising_data);
  if (result == 0) {
    return;
  }

  if (!update) result &= ~BTM_BLE_INQ_RESULT;

  tBTM_INQ_RESULTS_CB* p_inq_results_cb = p_inq->p_inq_results_cb;
  if (p_inq_results_cb && (result & BTM_BLE_INQ_RESULT)) {
    (p_inq_results_cb)((tBTM_INQ_RESULTS*)&p_i->inq_info.results,
                       const_cast<uint8_t*>(advertising_data.data()),
                       advertising_data.size());
  }
}

void btm_ble_process_phy_update_pkt(uint8_t len, uint8_t* data) {
  uint8_t status, tx_phy, rx_phy;
  uint16_t handle;

  LOG_ASSERT(len == 5);
  uint8_t* p = data;
  STREAM_TO_UINT8(status, p);
  STREAM_TO_UINT16(handle, p);
  handle = handle & 0x0FFF;
  STREAM_TO_UINT8(tx_phy, p);
  STREAM_TO_UINT8(rx_phy, p);

  gatt_notify_phy_updated(static_cast<tGATT_STATUS>(status), handle, tx_phy,
                          rx_phy);
}

/*******************************************************************************
 *
 * Function         btm_ble_start_scan
 *
 * Description      Start the BLE scan.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_start_scan() {
  tBTM_BLE_INQ_CB* p_inq = &btm_cb.ble_ctr_cb.inq_var;
  /* start scan, disable duplicate filtering */
  btm_send_hci_scan_enable(BTM_BLE_SCAN_ENABLE, BTM_BLE_DUPLICATE_DISABLE);

  if (p_inq->scan_type == BTM_BLE_SCAN_MODE_ACTI)
    btm_ble_set_topology_mask(BTM_BLE_STATE_ACTIVE_SCAN_BIT);
  else
    btm_ble_set_topology_mask(BTM_BLE_STATE_PASSIVE_SCAN_BIT);
}

/*******************************************************************************
 *
 * Function         btm_ble_stop_scan
 *
 * Description      Stop the BLE scan.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_stop_scan(void) {
  BTM_TRACE_EVENT("btm_ble_stop_scan ");

  if (btm_cb.ble_ctr_cb.inq_var.scan_type == BTM_BLE_SCAN_MODE_ACTI)
    btm_ble_clear_topology_mask(BTM_BLE_STATE_ACTIVE_SCAN_BIT);
  else
    btm_ble_clear_topology_mask(BTM_BLE_STATE_PASSIVE_SCAN_BIT);

  /* Clear the inquiry callback if set */
  btm_cb.ble_ctr_cb.inq_var.scan_type = BTM_BLE_SCAN_MODE_NONE;

  /* stop discovery now */
  btm_send_hci_scan_enable(BTM_BLE_SCAN_DISABLE, BTM_BLE_DUPLICATE_ENABLE);

  btm_update_scanner_filter_policy(SP_ADV_ALL);
}
/*******************************************************************************
 *
 * Function         btm_ble_stop_inquiry
 *
 * Description      Stop the BLE Inquiry.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_stop_inquiry(void) {
  tBTM_INQUIRY_VAR_ST* p_inq = &btm_cb.btm_inq_vars;
  tBTM_BLE_CB* p_ble_cb = &btm_cb.ble_ctr_cb;

  alarm_cancel(p_ble_cb->inq_var.inquiry_timer);

  p_ble_cb->reset_ble_inquiry();

  /* Cleanup anything remaining on index 0 */
  BTM_BleAdvFilterParamSetup(BTM_BLE_SCAN_COND_DELETE,
                             static_cast<tBTM_BLE_PF_FILT_INDEX>(0), nullptr,
                             base::Bind(btm_ble_scan_filt_param_cfg_evt));

  /* If no more scan activity, stop LE scan now */
  if (!p_ble_cb->is_ble_scan_active()) {
    btm_ble_stop_scan();
  } else if ((p_ble_cb->inq_var.scan_interval !=
              BTM_BLE_LOW_LATENCY_SCAN_INT) ||
             (p_ble_cb->inq_var.scan_window != BTM_BLE_LOW_LATENCY_SCAN_WIN)) {
    BTM_TRACE_DEBUG("%s: setting default params for ongoing observe", __func__);
    btm_ble_stop_scan();
    btm_ble_start_scan();
  }

  /* If we have a callback registered for inquiry complete, call it */
  BTM_TRACE_DEBUG("BTM Inq Compl Callback: status 0x%02x, num results %d",
                  p_inq->inq_cmpl_info.status, p_inq->inq_cmpl_info.num_resp);

  btm_process_inq_complete(
      HCI_SUCCESS, (uint8_t)(p_inq->inqparms.mode & BTM_BLE_INQUIRY_MASK));
}

/*******************************************************************************
 *
 * Function         btm_ble_stop_observe
 *
 * Description      Stop the BLE Observe.
 *
 * Returns          void
 *
 ******************************************************************************/
static void btm_ble_stop_observe(void) {
  tBTM_BLE_CB* p_ble_cb = &btm_cb.ble_ctr_cb;
  tBTM_CMPL_CB* p_obs_cb = p_ble_cb->p_obs_cmpl_cb;

  alarm_cancel(p_ble_cb->observer_timer);

  p_ble_cb->reset_ble_observe();

  p_ble_cb->p_obs_results_cb = NULL;
  p_ble_cb->p_obs_cmpl_cb = NULL;

  if (!p_ble_cb->is_ble_scan_active()) {
    btm_ble_stop_scan();
  }

  if (p_obs_cb) (p_obs_cb)(&btm_cb.btm_inq_vars.inq_cmpl_info);
}
/*******************************************************************************
 *
 * Function         btm_ble_adv_states_operation
 *
 * Description      Set or clear adv states in topology mask
 *
 * Returns          operation status. true if sucessful, false otherwise.
 *
 ******************************************************************************/
typedef bool(BTM_TOPOLOGY_FUNC_PTR)(tBTM_BLE_STATE_MASK);
static bool btm_ble_adv_states_operation(BTM_TOPOLOGY_FUNC_PTR* p_handler,
                                         uint8_t adv_evt) {
  bool rt = false;

  switch (adv_evt) {
    case BTM_BLE_CONNECT_EVT:
      rt = (*p_handler)(BTM_BLE_STATE_CONN_ADV_BIT);
      break;

    case BTM_BLE_NON_CONNECT_EVT:
      rt = (*p_handler)(BTM_BLE_STATE_NON_CONN_ADV_BIT);
      break;
    case BTM_BLE_CONNECT_DIR_EVT:
      rt = (*p_handler)(BTM_BLE_STATE_HI_DUTY_DIR_ADV_BIT);
      break;

    case BTM_BLE_DISCOVER_EVT:
      rt = (*p_handler)(BTM_BLE_STATE_SCAN_ADV_BIT);
      break;

    case BTM_BLE_CONNECT_LO_DUTY_DIR_EVT:
      rt = (*p_handler)(BTM_BLE_STATE_LO_DUTY_DIR_ADV_BIT);
      break;

    default:
      BTM_TRACE_ERROR("unknown adv event : %d", adv_evt);
      break;
  }

  return rt;
}

/*******************************************************************************
 *
 * Function         btm_ble_start_adv
 *
 * Description      start the BLE advertising.
 *
 * Returns          void
 *
 ******************************************************************************/
tBTM_STATUS btm_ble_start_adv(void) {
  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;

  if (!btm_ble_adv_states_operation(btm_ble_topology_check, p_cb->evt_type))
    return BTM_WRONG_MODE;

  btsnd_hcic_ble_set_adv_enable(BTM_BLE_ADV_ENABLE);
  p_cb->adv_mode = BTM_BLE_ADV_ENABLE;
  btm_ble_adv_states_operation(btm_ble_set_topology_mask, p_cb->evt_type);
  return BTM_SUCCESS;
}

/*******************************************************************************
 *
 * Function         btm_ble_stop_adv
 *
 * Description      Stop the BLE advertising.
 *
 * Returns          void
 *
 ******************************************************************************/
tBTM_STATUS btm_ble_stop_adv(void) {
  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;

  if (p_cb->adv_mode == BTM_BLE_ADV_ENABLE) {
    btsnd_hcic_ble_set_adv_enable(BTM_BLE_ADV_DISABLE);

    p_cb->fast_adv_on = false;
    p_cb->adv_mode = BTM_BLE_ADV_DISABLE;

    /* clear all adv states */
    btm_ble_clear_topology_mask(BTM_BLE_STATE_ALL_ADV_MASK);
  }
  return BTM_SUCCESS;
}

static void btm_ble_fast_adv_timer_timeout(UNUSED_ATTR void* data) {
  /* fast adv is completed, fall back to slow adv interval */
  btm_ble_start_slow_adv();
}

/*******************************************************************************
 *
 * Function         btm_ble_start_slow_adv
 *
 * Description      Restart adv with slow adv interval
 *
 * Returns          void
 *
 ******************************************************************************/
static void btm_ble_start_slow_adv(void) {
  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;

  if (p_cb->adv_mode == BTM_BLE_ADV_ENABLE) {
    tBTM_LE_RANDOM_CB* p_addr_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
    RawAddress address = RawAddress::kEmpty;
    tBLE_ADDR_TYPE init_addr_type = BLE_ADDR_PUBLIC;
    tBLE_ADDR_TYPE own_addr_type = p_addr_cb->own_addr_type;

    btm_ble_stop_adv();

    p_cb->evt_type = btm_set_conn_mode_adv_init_addr(address, &init_addr_type,
                                                     &own_addr_type);

    /* slow adv mode never goes into directed adv */
    btsnd_hcic_ble_write_adv_params(
        BTM_BLE_GAP_ADV_SLOW_INT, BTM_BLE_GAP_ADV_SLOW_INT, p_cb->evt_type,
        own_addr_type, init_addr_type, address, p_cb->adv_chnl_map, p_cb->afp);

    btm_ble_start_adv();
  }
}

static void btm_ble_inquiry_timer_gap_limited_discovery_timeout(
    UNUSED_ATTR void* data) {
  /* lim_timeout expired, limited discovery should exit now */
  btm_cb.btm_inq_vars.discoverable_mode &= ~BTM_BLE_LIMITED_DISCOVERABLE;
  btm_ble_set_adv_flag(btm_cb.btm_inq_vars.connectable_mode,
                       btm_cb.btm_inq_vars.discoverable_mode);
}

static void btm_ble_inquiry_timer_timeout(UNUSED_ATTR void* data) {
  btm_ble_stop_inquiry();
}

static void btm_ble_observer_timer_timeout(UNUSED_ATTR void* data) {
  btm_ble_stop_observe();
}

/*******************************************************************************
 *
 * Function         btm_ble_read_remote_features_complete
 *
 * Description      This function is called when the command complete message
 *                  is received from the HCI for the read LE remote feature
 *                  supported complete event.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_read_remote_features_complete(uint8_t* p) {
  uint16_t handle;
  uint8_t status;
  STREAM_TO_UINT8(status, p);
  STREAM_TO_UINT16(handle, p);
  handle = handle & 0x0FFF;  // only 12 bits meaningful

  if (status != HCI_SUCCESS) {
    if (status != HCI_ERR_UNSUPPORTED_REM_FEATURE) {
      LOG_ERROR("Failed to read remote features status:%s",
                hci_error_code_text(static_cast<tHCI_STATUS>(status)).c_str());
      return;
    }
    LOG_WARN("Remote does not support reading remote feature");
  }

  if (status == HCI_SUCCESS) {
    if (!acl_set_peer_le_features_from_handle(handle, p)) {
      LOG_ERROR(
          "Unable to find existing connection after read remote features");
      return;
    }
  }

  btsnd_hcic_rmt_ver_req(handle);
}

/*******************************************************************************
 *
 * Function         btm_ble_write_adv_enable_complete
 *
 * Description      This function process the write adv enable command complete.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_write_adv_enable_complete(uint8_t* p) {
  tBTM_BLE_INQ_CB* p_cb = &btm_cb.ble_ctr_cb.inq_var;

  /* if write adv enable/disbale not succeed */
  if (*p != HCI_SUCCESS) {
    /* toggle back the adv mode */
    p_cb->adv_mode = !p_cb->adv_mode;
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_dir_adv_tout
 *
 * Description      when directed adv time out
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_dir_adv_tout(void) {
  btm_cb.ble_ctr_cb.inq_var.adv_mode = BTM_BLE_ADV_DISABLE;

  /* make device fall back into undirected adv mode by default */
  btm_cb.ble_ctr_cb.inq_var.directed_conn = BTM_BLE_ADV_IND_EVT;
}

/*******************************************************************************
 *
 * Function         btm_ble_set_topology_mask
 *
 * Description      set BLE topology mask
 *
 * Returns          true is request is allowed, false otherwise.
 *
 ******************************************************************************/
bool btm_ble_set_topology_mask(tBTM_BLE_STATE_MASK request_state_mask) {
  request_state_mask &= BTM_BLE_STATE_ALL_MASK;
  btm_cb.ble_ctr_cb.cur_states |= (request_state_mask & BTM_BLE_STATE_ALL_MASK);
  return true;
}

/*******************************************************************************
 *
 * Function         btm_ble_clear_topology_mask
 *
 * Description      Clear BLE topology bit mask
 *
 * Returns          true is request is allowed, false otherwise.
 *
 ******************************************************************************/
bool btm_ble_clear_topology_mask(tBTM_BLE_STATE_MASK request_state_mask) {
  request_state_mask &= BTM_BLE_STATE_ALL_MASK;
  btm_cb.ble_ctr_cb.cur_states &= ~request_state_mask;
  return true;
}

/*******************************************************************************
 *
 * Function         btm_ble_update_link_topology_mask
 *
 * Description      This function update the link topology mask
 *
 * Returns          void
 *
 ******************************************************************************/
static void btm_ble_update_link_topology_mask(uint8_t link_role,
                                              bool increase) {
  btm_ble_clear_topology_mask(BTM_BLE_STATE_ALL_CONN_MASK);

  if (increase)
    btm_cb.ble_ctr_cb.link_count[link_role]++;
  else if (btm_cb.ble_ctr_cb.link_count[link_role] > 0)
    btm_cb.ble_ctr_cb.link_count[link_role]--;

  if (btm_cb.ble_ctr_cb.link_count[HCI_ROLE_CENTRAL])
    btm_ble_set_topology_mask(BTM_BLE_STATE_CENTRAL_BIT);

  if (btm_cb.ble_ctr_cb.link_count[HCI_ROLE_PERIPHERAL])
    btm_ble_set_topology_mask(BTM_BLE_STATE_PERIPHERAL_BIT);

  if (link_role == HCI_ROLE_PERIPHERAL && increase) {
    btm_cb.ble_ctr_cb.inq_var.adv_mode = BTM_BLE_ADV_DISABLE;
    /* make device fall back into undirected adv mode by default */
    btm_cb.ble_ctr_cb.inq_var.directed_conn = BTM_BLE_ADV_IND_EVT;
    /* clear all adv states */
    btm_ble_clear_topology_mask(BTM_BLE_STATE_ALL_ADV_MASK);
  }
}

void btm_ble_increment_link_topology_mask(uint8_t link_role) {
  btm_ble_update_link_topology_mask(link_role, true);
}

void btm_ble_decrement_link_topology_mask(uint8_t link_role) {
  btm_ble_update_link_topology_mask(link_role, false);
}

/*******************************************************************************
 *
 * Function         btm_ble_update_mode_operation
 *
 * Description      This function update the GAP role operation when a link
 *                  status is updated.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_update_mode_operation(uint8_t link_role, const RawAddress* bd_addr,
                                   tHCI_STATUS status) {
  if (status == HCI_ERR_ADVERTISING_TIMEOUT) {
    btm_cb.ble_ctr_cb.inq_var.adv_mode = BTM_BLE_ADV_DISABLE;
    /* make device fall back into undirected adv mode by default */
    btm_cb.ble_ctr_cb.inq_var.directed_conn = BTM_BLE_ADV_IND_EVT;
    /* clear all adv states */
    btm_ble_clear_topology_mask(BTM_BLE_STATE_ALL_ADV_MASK);
  }

  if (btm_cb.ble_ctr_cb.inq_var.connectable_mode == BTM_BLE_CONNECTABLE) {
    btm_ble_set_connectability(btm_cb.btm_inq_vars.connectable_mode |
                               btm_cb.ble_ctr_cb.inq_var.connectable_mode);
  }

  /* in case of disconnected, we must cancel bgconn and restart
     in order to add back device to acceptlist in order to reconnect */
  if (bd_addr != nullptr) {
      LOG_DEBUG("gd_acl enabled so skip background connection logic");
  }

  /* when no connection is attempted, and controller is not rejecting last
     request
     due to resource limitation, start next direct connection or background
     connection
     now in order */
  if (btm_cb.ble_ctr_cb.is_connection_state_idle() &&
      status != HCI_ERR_HOST_REJECT_RESOURCES &&
      status != HCI_ERR_MAX_NUM_OF_CONNECTIONS) {
    LOG_DEBUG("Resuming le background connections");
    btm_ble_resume_bg_conn();
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_init
 *
 * Description      Initialize the control block variable values.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_init(void) {
  tBTM_BLE_CB* p_cb = &btm_cb.ble_ctr_cb;

  BTM_TRACE_DEBUG("%s", __func__);

  alarm_free(p_cb->observer_timer);
  alarm_free(p_cb->inq_var.fast_adv_timer);
  memset(p_cb, 0, sizeof(tBTM_BLE_CB));
  memset(&(btm_cb.cmn_ble_vsc_cb), 0, sizeof(tBTM_BLE_VSC_CB));
  btm_cb.cmn_ble_vsc_cb.values_read = false;

  p_cb->observer_timer = alarm_new("btm_ble.observer_timer");
  p_cb->cur_states = 0;

  p_cb->inq_var.adv_mode = BTM_BLE_ADV_DISABLE;
  p_cb->inq_var.scan_type = BTM_BLE_SCAN_MODE_NONE;
  p_cb->inq_var.adv_chnl_map = BTM_BLE_DEFAULT_ADV_CHNL_MAP;
  p_cb->inq_var.afp = BTM_BLE_DEFAULT_AFP;
  p_cb->inq_var.sfp = BTM_BLE_DEFAULT_SFP;
  p_cb->inq_var.connectable_mode = BTM_BLE_NON_CONNECTABLE;
  p_cb->inq_var.discoverable_mode = BTM_BLE_NON_DISCOVERABLE;
  p_cb->inq_var.fast_adv_timer = alarm_new("btm_ble_inq.fast_adv_timer");
  p_cb->inq_var.inquiry_timer = alarm_new("btm_ble_inq.inquiry_timer");

  /* for background connection, reset connection params to be undefined */
  p_cb->scan_int = p_cb->scan_win = BTM_BLE_SCAN_PARAM_UNDEF;

  p_cb->inq_var.evt_type = BTM_BLE_NON_CONNECT_EVT;

  p_cb->addr_mgnt_cb.refresh_raddr_timer =
      alarm_new("btm_ble_addr.refresh_raddr_timer");
  btm_ble_pa_sync_cb = {};
  sync_timeout_alarm = alarm_new("btm.sync_start_task");
#if (BLE_VND_INCLUDED == FALSE)
  btm_ble_adv_filter_init();
#endif
}

// Clean up btm ble control block
void btm_ble_free() {
  tBTM_BLE_CB* p_cb = &btm_cb.ble_ctr_cb;
  alarm_free(p_cb->addr_mgnt_cb.refresh_raddr_timer);
}

/*******************************************************************************
 *
 * Function         btm_ble_topology_check
 *
 * Description      check to see requested state is supported. One state check
 *                  at a time is supported
 *
 * Returns          true is request is allowed, false otherwise.
 *
 ******************************************************************************/
bool btm_ble_topology_check(tBTM_BLE_STATE_MASK request_state_mask) {
  bool rt = false;

  uint8_t state_offset = 0;
  uint16_t cur_states = btm_cb.ble_ctr_cb.cur_states;
  uint8_t request_state = 0;

  /* check only one bit is set and within valid range */
  if (request_state_mask == BTM_BLE_STATE_INVALID ||
      request_state_mask > BTM_BLE_STATE_SCAN_ADV_BIT ||
      (request_state_mask & (request_state_mask - 1)) != 0) {
    BTM_TRACE_ERROR("illegal state requested: %d", request_state_mask);
    return rt;
  }

  while (request_state_mask) {
    request_state_mask >>= 1;
    request_state++;
  }

  /* check if the requested state is supported or not */
  uint8_t bit_num = btm_le_state_combo_tbl[0][request_state - 1];
  const uint8_t* ble_supported_states =
      controller_get_interface()->get_ble_supported_states();

  if (!BTM_LE_STATES_SUPPORTED(ble_supported_states, bit_num)) {
    BTM_TRACE_ERROR("state requested not supported: %d", request_state);
    return rt;
  }

  rt = true;
  /* make sure currently active states are all supported in conjunction with the
     requested state. If the bit in table is UNSUPPORTED, the combination is not
     supported */
  while (cur_states != 0) {
    if (cur_states & 0x01) {
      uint8_t bit_num = btm_le_state_combo_tbl[request_state][state_offset];
      if (bit_num != UNSUPPORTED) {
        if (!BTM_LE_STATES_SUPPORTED(ble_supported_states, bit_num)) {
          rt = false;
          break;
        }
      }
    }
    cur_states >>= 1;
    state_offset++;
  }
  return rt;
}
