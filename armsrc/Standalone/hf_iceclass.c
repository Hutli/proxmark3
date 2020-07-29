//-----------------------------------------------------------------------------
// Christian Herrmann, 2020
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// main code for hf_iceclass by Iceman
//-----------------------------------------------------------------------------
#include "standalone.h" // standalone definitions
#include "proxmark3_arm.h"
#include "appmain.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "util.h"
#include "dbprint.h"
#include "spiffs.h"
#include "iclass.h"
#include "optimized_cipher.h"

#define NUM_CSNS                    9
#define MAC_RESPONSES_SIZE          (16 * NUM_CSNS)
#define HF_ICLASS_FULLSIM_ORIG_BIN  "iceclass-orig.bin"
#define HF_ICLASS_FULLSIM_MOD       "iceclass-modified"
#define HF_ICLASS_FULLSIM_MOD_BIN   HF_ICLASS_FULLSIM_MOD".bin"
#define HF_ICLASS_FULLSIM_MOD_EML   HF_ICLASS_FULLSIM_MOD".eml"
#define HF_ICLASS_ATTACK_BIN        "iclass_mac_attack.bin"

#define HF_ICLASS_CC_A              "iceclass_cc_a.bin"
#define HF_ICLASS_CC_B              "iceclass_cc_b.bin"
char* cc_files[] = { HF_ICLASS_CC_A, HF_ICLASS_CC_B };

#define ICE_STATE_NONE        0
#define ICE_STATE_FULLSIM     1
#define ICE_STATE_ATTACK      2
#define ICE_STATE_READER      3
#define ICE_STATE_CONFIGCARD  4

typedef struct {
    uint8_t app_limit;      //[8]
    uint8_t otp[2];         //[9-10]
    uint8_t block_writelock;//[11]
    uint8_t chip_config;    //[12]
    uint8_t mem_config;     //[13]
    uint8_t eas;            //[14]
    uint8_t fuses;          //[15]
} picopass_conf_block_t;

// iclass card descriptors
char * card_types[] = {
    "PicoPass 16K / 16",                       // 000
    "PicoPass 32K with current book 16K / 16", // 001
    "Unknown Card Type!",                      // 010
    "Unknown Card Type!",                      // 011
    "PicoPass 2K",                             // 100
    "Unknown Card Type!",                      // 101
    "PicoPass 16K / 2",                        // 110
    "PicoPass 32K with current book 16K / 2",  // 111
};

uint8_t card_app2_limit[] = {
    0xff,
    0xff,
    0xff,
    0xff,
    0x1f,
    0xff,
    0xff,
    0xff,
};

static uint8_t aa2_key[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t legacy_aa1_key[] = {0xAE, 0xA6, 0x84, 0xA6, 0xDA, 0xB2, 0x32, 0x78};

static uint8_t csns[8 * NUM_CSNS] = {
    0x01, 0x0A, 0x0F, 0xFF, 0xF7, 0xFF, 0x12, 0xE0,
    0x0C, 0x06, 0x0C, 0xFE, 0xF7, 0xFF, 0x12, 0xE0,
    0x10, 0x97, 0x83, 0x7B, 0xF7, 0xFF, 0x12, 0xE0,
    0x13, 0x97, 0x82, 0x7A, 0xF7, 0xFF, 0x12, 0xE0,
    0x07, 0x0E, 0x0D, 0xF9, 0xF7, 0xFF, 0x12, 0xE0,
    0x14, 0x96, 0x84, 0x76, 0xF7, 0xFF, 0x12, 0xE0,
    0x17, 0x96, 0x85, 0x71, 0xF7, 0xFF, 0x12, 0xE0,
    0xCE, 0xC5, 0x0F, 0x77, 0xF7, 0xFF, 0x12, 0xE0,
    0xD2, 0x5A, 0x82, 0xF8, 0xF7, 0xFF, 0x12, 0xE0
    //0x04, 0x08, 0x9F, 0x78, 0x6E, 0xFF, 0x12, 0xE0
};

static void download_instructions(uint8_t t) {
    Dbprintf("");
    switch (t) { 
        case ICE_STATE_FULLSIM: {
            Dbprintf("The emulator memory was saved to flash. Try the following from flash and display it");
            Dbprintf("1. " _YELLOW_("mem spiffs dump o "HF_ICLASS_FULLSIM_MOD_BIN" f "HF_ICLASS_FULLSIM_MOD" e"));
            Dbprintf("2. " _YELLOW_("exit proxmark3 client"));
            Dbprintf("3. " _YELLOW_("cat "HF_ICLASS_FULLSIM_MOD_EML));
            break;
        } 
        case ICE_STATE_ATTACK: {
            Dbprintf("The emulator memory was saved to flash. Try the following from flash and display it");
            Dbprintf("1. " _YELLOW_("mem spiffs dump o "HF_ICLASS_ATTACK_BIN" f "HF_ICLASS_ATTACK_BIN));
            Dbprintf("2. " _YELLOW_("hf iclass loclass f "HF_ICLASS_ATTACK_BIN));
            break;
        }
        case ICE_STATE_READER: {
            Dbprintf("The found tags was saved to flash. Try to download from flash and display it");
            Dbprintf("1. " _YELLOW_("mem spiffs tree"));
            Dbprintf("2. " _YELLOW_("mem spiffs dump h"));
            break;
        }        
    }
}

static void save_to_flash(uint8_t *data, uint16_t datalen) {

    rdv40_spiffs_lazy_mount();

    char fn[SPIFFS_OBJ_NAME_LEN];
    sprintf(fn, "iclass-%02X%02X%02X%02X%02X%02X%02X%02X.bin",
        data[0], data[1], data[2], data[3],
        data[4], data[5], data[6], data[7]
    );

    if (exists_in_spiffs(fn) == false) {
        int res = rdv40_spiffs_write(fn, data, datalen, RDV40_SPIFFS_SAFETY_SAFE);
        if (res == SPIFFS_OK) {
            Dbprintf("Saved to `" _YELLOW_("%s") "`", fn);
        } else {
            Dbprintf("error writing `" _YELLOW_("%s") "`", fn);
        } 
    }    

    rdv40_spiffs_lazy_unmount();
}

static int fullsim_mode(void) {

    rdv40_spiffs_lazy_mount();
  
    SpinOff(0);
    uint8_t *emul = BigBuf_get_EM_addr();
    uint32_t fsize = size_in_spiffs(HF_ICLASS_FULLSIM_ORIG_BIN);
    int res = rdv40_spiffs_read_as_filetype(HF_ICLASS_FULLSIM_ORIG_BIN, emul, fsize, RDV40_SPIFFS_SAFETY_SAFE);
    rdv40_spiffs_lazy_unmount();
    if (res == SPIFFS_OK) {
        Dbprintf("loaded '" _YELLOW_(HF_ICLASS_FULLSIM_ORIG_BIN) "' (%u bytes) to emulator memory", fsize);
    }

    // create diversified key AA1/KD if not in dump.
    if ( memcmp(emul + (3 * 8), "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8) == 0) {
        uint8_t ccnr[12] = {0};
        memcpy(ccnr, emul + (2 * 8), 8);                   
        bool use_elite = false;
        iclass_calc_div_key(emul, legacy_aa1_key, emul + (3 * 8), use_elite);       
    }

    // create diversified key AA2/KC if not in dump.
    if ( memcmp(emul + (4 * 8), "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8) == 0) {
        uint8_t ccnr[12] = {0};
        memcpy(ccnr, emul + (2 * 8), 8);                   
        bool use_elite = false;
        iclass_calc_div_key(emul, aa2_key, emul + (4 * 8), use_elite);       
    }

    iclass_simulate(ICLASS_SIM_MODE_FULL, 0 , false, NULL, NULL, NULL);
    
    LED_B_ON();
    rdv40_spiffs_lazy_mount();
    res = rdv40_spiffs_write(HF_ICLASS_FULLSIM_MOD_BIN, emul, fsize, RDV40_SPIFFS_SAFETY_SAFE);
    rdv40_spiffs_lazy_unmount();
    LED_B_OFF();
    if (res != SPIFFS_OK) {
        Dbprintf("error writing '"HF_ICLASS_FULLSIM_MOD_BIN"' to flash ( %d )", res);
    }

    return PM3_SUCCESS;
}

static int reader_attack_mode(void) {

    BigBuf_free();
    uint16_t mac_response_len = 0;
    uint8_t *mac_responses = BigBuf_malloc(MAC_RESPONSES_SIZE);

    iclass_simulate(ICLASS_SIM_MODE_READER_ATTACK, NUM_CSNS, false, csns, mac_responses, &mac_response_len);

    if (mac_response_len > 0) {
        
        bool success = (mac_response_len == MAC_RESPONSES_SIZE);
        uint8_t num_mac = (mac_response_len >> 4);
        Dbprintf("%u out of %d MAC obtained [%s]", num_mac, NUM_CSNS, (success) ? _GREEN_("OK") : _RED_("FAIL"));

        size_t dumplen = NUM_CSNS * 24;

        uint8_t *dump = BigBuf_malloc(dumplen);
        if (dump == false) {
            Dbprintf("failed to allocate memory");
            return PM3_EMALLOC;
        }

        memset(dump, 0, dumplen);//<-- Need zeroes for the EPURSE - field

        for (uint8_t i = 0 ; i < NUM_CSNS ; i++) {
            //copy CSN
            memcpy(dump + (i * 24), csns + (i * 8), 8);
            //copy epurse
            memcpy(dump + (i * 24) + 8, mac_responses + (i * 16), 8);
            // NR_MAC (eight bytes from the response)  ( 8b csn + 8b epurse == 16)
            memcpy(dump + (i * 24) + 16, mac_responses + (i * 16) + 8, 8);
        }

        LED_B_ON();
        rdv40_spiffs_lazy_mount();
        int res = rdv40_spiffs_write(HF_ICLASS_ATTACK_BIN, dump, dumplen, RDV40_SPIFFS_SAFETY_SAFE);
        rdv40_spiffs_lazy_unmount();
        LED_B_OFF();
        if (res != SPIFFS_OK) {
            Dbprintf("error writing '"HF_ICLASS_ATTACK_BIN"' to flash ( %d )", res);
        }
    }
    return PM3_SUCCESS;
}

static int reader_dump_mode(void) {
    
    BigBuf_free();
    uint8_t *card_data = BigBuf_malloc(0xFF * 8);
    memset(card_data, 0xFF, sizeof(card_data));

    struct p {
        uint8_t key[8];
        bool use_raw;
        bool use_elite;
        bool use_credit_key;
    } PACKED;

    for (;;) {
        
        if (BUTTON_PRESS()) {
            DbpString("button pressed");
            break;
        }

        // AA1 
        struct p payload = {
            .use_raw = false,
            .use_elite = false,
            .use_credit_key = false,
        };
        memcpy(payload.key, legacy_aa1_key, sizeof(payload.key));

        bool isOK = iclass_auth((uint8_t*)&payload, false, card_data);        
        if (isOK == false) {
            continue;
        }
        
        picopass_conf_block_t *conf = (picopass_conf_block_t*)(card_data + 8);

        // get 3 config bits
        uint8_t type = (conf->chip_config & 0x10) >> 2;
        type |= (conf->mem_config & 0x80) >> 6;
        type |= (conf->mem_config & 0x20) >> 5;

        uint8_t app1_limit = conf->app_limit - 5; // minus header blocks
        uint8_t app2_limit = card_app2_limit[type];
        
        
        uint16_t dumped = 0;
        uint8_t block;
        for (block = 5; block < app1_limit; block++) {
            isOK = iclass_readblock(block, card_data + (8 * block));
            if (isOK) {
                dumped++;
            }
        }

        // AA2 
        payload.use_credit_key = true;
        memcpy(payload.key, aa2_key, sizeof(payload.key));

        isOK = iclass_auth((uint8_t*)&payload, false, card_data);        
        if (isOK) {
            for (; block < app2_limit; block++) {
                isOK = iclass_readblock(block, card_data + (8 * block));
                if (isOK) {
                    dumped++;
                }
            }
        }

        Dbprintf("Found %s", card_types[type]);
/*
        Dbprintf("APP1 Blocks: %d", app1_limit); 
        Dbprintf("APP2 Blocks: %d", app2_limit - app1_limit - 5); // minus app1 and header
        Dbprintf("Got %d blocks (saving %u, %u bytes )", dumped, dumped + 5, ((dumped+5)*8) ); 
*/
        if (5 + dumped > app1_limit) {
            save_to_flash(card_data, (5 + dumped) * 8 );        
        }        
    }

    Dbprintf("exit read & dump mode");
    return PM3_SUCCESS;
}

static int config_sim_mode(void) {

    uint8_t *emul = BigBuf_get_EM_addr();

    for (uint8_t i = 0; i < 2; i++) {
        SpinOff(0);
        
        rdv40_spiffs_lazy_mount();
        uint32_t fsize = size_in_spiffs(cc_files[i]);
        int res = rdv40_spiffs_read_as_filetype(cc_files[i], emul, fsize, RDV40_SPIFFS_SAFETY_SAFE);
        rdv40_spiffs_lazy_unmount();

        if (res == SPIFFS_OK) {
            Dbprintf("loaded '" _YELLOW_("%s") "' (%u bytes) to emulator memory", cc_files[i], fsize);
            iclass_simulate(ICLASS_SIM_MODE_FULL, 0 , false, NULL, NULL, NULL);
        }
    }

    return PM3_SUCCESS;
}

void ModInfo(void) {
    DbpString("  HF iCLASS mode -  aka iceCLASS (iceman)");
}

void RunMod(void) {

    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    BigBuf_Clear();

    StandAloneMode();
    Dbprintf(_YELLOW_("HF iCLASS mode a.k.a iceCLASS started"));

    uint8_t mode = ICE_STATE_FULLSIM;

    for (;;) {

        WDT_HIT();

        if (mode == ICE_STATE_NONE) break;
        if (data_available()) break;

/*
        // Was our button held down or pressed?
        int button_pressed = BUTTON_HELD(1000);
        if (button_pressed != BUTTON_NO_CLICK) {
            break;
        }
        */
                
        int res;
        switch (mode) {

            case ICE_STATE_FULLSIM: {
                Dbprintf("enter full simulation mode");

                // Look for iCLASS dump file
                rdv40_spiffs_lazy_mount();
                if (exists_in_spiffs(HF_ICLASS_FULLSIM_ORIG_BIN) == false) {
                    Dbprintf("error, '" _YELLOW_(HF_ICLASS_FULLSIM_ORIG_BIN) "' file missing");
                    mode = ICE_STATE_NONE;
                }
                rdv40_spiffs_lazy_unmount();
                
                if (mode == ICE_STATE_FULLSIM) {
                    res = fullsim_mode();
                    if (res == PM3_SUCCESS) {
                        download_instructions(mode);
                    }
                }
                // the button press to exit sim, is captured in main loop here
                mode = ICE_STATE_NONE;
                break;
            }
            case ICE_STATE_ATTACK: {
                Dbprintf("enter reader attack mode");
                res = reader_attack_mode();
                if (res == PM3_SUCCESS)
                    download_instructions(mode);

                mode = ICE_STATE_NONE;
                break;
            }
            case ICE_STATE_READER: {
                Dbprintf("enter read & dump mode");
                res = reader_dump_mode();
                if (res == PM3_SUCCESS)
                    download_instructions(mode);

                mode = ICE_STATE_NONE;
                break;
            }
            case ICE_STATE_CONFIGCARD: {
                Dbprintf("enter config card simulation mode");

                // Look for config cards                 
                rdv40_spiffs_lazy_mount();
                for (uint8_t i =0; i < 2; i++) {
                    if (exists_in_spiffs(cc_files[i]) == false) {
                        Dbprintf("error, '" _YELLOW_("%s") "' file missing", cc_files[i]);
                        mode = ICE_STATE_NONE;
                    }
                }
                rdv40_spiffs_lazy_unmount();
                
                if (mode == ICE_STATE_CONFIGCARD)
                    config_sim_mode();

                mode = ICE_STATE_NONE;
                break;
            }
        }
    }

    switch_off();
    Dbprintf("-=[ exit iceCLASS ]=-");
}