// Simple GB/GBC/MD/CB Game Cartridge Explorer
//          /w Interactive Command Interpreter
// (C) 2026 NR RetroWorks, MIT Licensed
//
// Modes of Operation:
// -  /w Serial Monitor: Interactive Mode - Command interpreter available for issuing commands to cartridge.
// - w/o Serial Monitor: Standalone Mode - Attempts to program from "/rom.bin", if available from SD card.
//
// Command Interpreter:
//  Basic Commands:
//  HELP                  -> Help menu
//  STATUS                -> Status readout (/w full refresh)
//  RESET                 -> Device reset (done automatically on startup)
//  
//  Pin Commands:
//  CLK <0|1>             -> Asserts/deasserts CLK (asserts/syncs on falling edge)
//  WR <0|1>              -> Asserts/deasserts /WE
//  RD <0|1>              -> Asserts/deasserts /RD
//  CS <0|1>              -> Asserts/deasserts /CS (only for RAM enable override)
//  DEASSERT              -> Deasserts all control lines
//  ADDR                  -> Displays current ADDR (/w refresh)
//  ADDR <addr>           -> Sets address to ADDR
//  DATA                  -> Displays current DATA (/w refresh)
//  DATA <data>           -> Sets data to DATA (forces output mode - may contend)
//  
//  Bus Commands:
//  READ  <addr>          -> Reads from ADDR
//  WRITE <addr> <data>   -> Writes DATA to ADDR
//  
//  ROM Commands:
//  BANK <hex>            -> Sets ROM bank # (>0xFF req. MBC5 compat.)
//  SIZE <bytes>          -> Sets size of ROM for dumps (default: 4MB)
//  ID                    -> Displays ROM Manufacturer/Device ID
//  ERASE CHIP            -> Erases entire ROM chip to 0xFF
//  ERASE <sector>        -> Erases specific ROM sector to 0xFF
//  PROG <addr> <data>    -> Programs DATA to ADDR
//  PROG <file>           -> Programs ROM from SD file
//  DUMP <file>           -> Dumps ROM to SD file (requires SIZE set)
//  
//  RAM Commands:
//  RAM <ON|OFF>          -> Sets RAM on/off
//  RAMBANK <hex>         -> Sets RAM bank #
//  RAMSIZE <bytes>       -> Sets size of RAM for dumps/blanking (default: 32KB)
//  RAMBLANK              -> Resets entire RAM chip to 0x00 (requires RAMSIZE set)
//  RAMPROG <file>        -> Programs RAM from SD file
//  RAMDUMP <file>        -> Dumps RAM to SD file (requires RAMSIZE set)
//  
//  Address Map:
//       ROM BANK 0:      0x0000-0x3FFF (locked to bank 0)
//     ROM BANK 1/X:      0x4000-0x7FFF (settable bank)
//              RAM:      0xA000-0xBFFF (settable bank)
//  
//  LED Status:
//         Solid On:      Programming "/rom.bin" completed (standalone mode)
//       Fast Blink:      Failure programming "/rom.bin" (standalone mode)
//  Very Fast Blink:      Programming/erasing chip activity
//       Slow Blink:      Ready for commands (interactive mode)
//        Solid Off:      Not yet initialized / hardware fault
//  
//  ROM Short-Hand Sizes: 256,512,1,2,4,8,16,32,64,128 (256Kbit to 128Mbit)
//  RAM Short-Hand Sizes: 2,4,8,16,32,64,128,256,512,1 (2Kbit to 1Mbit)

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

// Address Pins
const uint8_t PIN_ADDR[16] =
{
  //A0,A1,A2,A3,A4,A5,A6,A7,A8,A9,A10,A11,A12,A13,A14,A15
    22,24,26,28,30,32,34,36,38,40, 42, 44, 46, 48, 39, 41
};

// Data Pins
const uint8_t PIN_DATA[8] = {
  //D0,D1,D2,D3,D4,D5,D6,D7
    23,25,27,29,31,33,35,37
};

// Control Pins
const int PIN_CLK = 43;
const int PIN_WR  = 45;
const int PIN_RD  = 47;
const int PIN_CS  = 49;
const int PIN_RST = 2;
const int PIN_SD_CS = 53;
const int PIN_LED = 13;

// You may need to adjust these to match your chip's specific codes. Refer to your chip's datasheet.
const uint16_t FLASH_UNLOCK_ADDR_1   = 0x5555;    // Flash unlock address 1 (AM/SST style)
const uint16_t FLASH_UNLOCK_DATA_1   = 0xAA;      // Flash unlock data 1 (AM/SST style)
const uint16_t FLASH_UNLOCK_ADDR_2   = 0x2AAA;    // Flash unlock address 2 (AM/SST style)
const uint16_t FLASH_UNLOCK_DATA_2   = 0x55;      // Flash unlock data 2 (AM/SST style)
const uint16_t FLASH_PROGRAM_CMD     = 0xA0;      // Flash program command data (AM/SST style)
const uint16_t FLASH_ERASE_CMD       = 0x80;      // Flash erase command data (AM/SST style)
const uint16_t FLASH_SECT_ERASE_CMD  = 0x30;      // Flash sector erase command data (AM/SST style)
const uint16_t FLASH_CHIP_ERASE_CMD  = 0x10;      // Flash chip erase command data (AM/SST style)
const uint16_t FLASH_ID_ENTRY_CMD    = 0x90;      // Flash chip ID entry command data (AM/SST style)
const uint16_t FLASH_ID_EXIT_CMD     = 0xF0;      // Flash chip ID exit command data (AM/SST style)

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

uint16_t rom_bank = 1;                            // Current rom bank (shadowed)
uint8_t ram_bank = 0;                             // Current ram bank (shadowed)
bool ram_enabled = false;                         // RAM enable flag (shadowed)
uint16_t curr_addr = 0;                           // Current ADDR (shadowed)
uint8_t curr_data = 0;                            // Current DATA (shadowed)
uint32_t rom_size = 4UL * 1024UL * 1024UL;        // ROM total bytes, default 4MB
uint32_t ram_size = 32UL * 1024UL;                // RAM total bytes, default 32KB
bool interactive_mode = true;                     // Interactive Serial mode flag
bool looped_once = false;                         // Run once flag

const unsigned int SYNC_ON_CLK       = 0x01;      // Syncs on CLK (assertion triggers latch capture)
const unsigned int SYNC_ON_WR        = 0x02;      // Syncs on /WR (assertion triggers latch capture)
const unsigned int SYNC_ON_RD        = 0x04;      // Syncs on /RD (assertion triggers latch capture)
const unsigned int SYNC_ON_CS        = 0x08;      // Syncs on /CS (assertion triggers latch capture)

const unsigned int MAX_ERROR_COUNT   = 8;         // Verify failures before quit
const unsigned int MAX_FLASH_RETRIES = 16;        // Retry verify time limit/attempts
const unsigned int ADDR_BLINK_DIV    = 128;       // Bytes processed between blink reversals
const unsigned int ROM_START         = 0x0000;    // 0x0000-0x3FFF (BANK 0), 0x4000-0x7FFF (BANK 1/X)
const unsigned int ROM_BANK_SIZE     = 0x4000;    // 16KB
const unsigned int RAM_START         = 0xA000;    // 0xA000-0xBFFF
const unsigned int RAM_BANK_SIZE     = 0x2000;    // 8KB

// Short-hand ROM sizes (in Kbit/Mbit)
const unsigned int ROM_SIZE_256KBIT = 256;
const unsigned int ROM_SIZE_512KBIT = 512;
const unsigned int ROM_SIZE_1MBIT   = 1;
const unsigned int ROM_SIZE_2MBIT   = 2;
const unsigned int ROM_SIZE_4MBIT   = 4;
const unsigned int ROM_SIZE_8MBIT   = 8;
const unsigned int ROM_SIZE_16MBIT  = 16;
const unsigned int ROM_SIZE_32MBIT  = 32;
const unsigned int ROM_SIZE_64MBIT  = 64;
const unsigned int ROM_SIZE_128MBIT = 128;

// Short-hand RAM sizes (in Kbit/Mbit)
const unsigned int RAM_SIZE_2KBIT   = 2;
const unsigned int RAM_SIZE_4KBIT   = 4;
const unsigned int RAM_SIZE_8KBIT   = 8;
const unsigned int RAM_SIZE_16KBIT  = 16;
const unsigned int RAM_SIZE_32KBIT  = 32;
const unsigned int RAM_SIZE_64KBIT  = 64;
const unsigned int RAM_SIZE_128KBIT = 128;
const unsigned int RAM_SIZE_256KBIT = 256;
const unsigned int RAM_SIZE_512KBIT = 512;
const unsigned int RAM_SIZE_1MBIT   = 1;

// -----------------------------------------------------------------------------
// Bus Interface
// -----------------------------------------------------------------------------

void setDataInput();
void setAddress(uint16_t addr);
uint16_t getAddress();
void setData(uint8_t data);
uint8_t getData();
void setBus(uint16_t address, uint8_t data);

void assertClock();
void deassertClock();
void assertRead();
void deassertRead();
void assertWrite();
void deassertWrite();
void assertCableSelect();
void deassertCableSelect();
void deassertAllControls();

void resetDevice();

uint32_t mapAddress(uint16_t addr);
void demapAddress(uint32_t physAddr, uint8_t &bank, uint16_t &offset);
uint32_t mapRAMAddress(uint16_t addr);
void demapRAMAddress(uint32_t physAddr, uint8_t &bank, uint16_t &offset);

void busWrite(uint16_t addr, uint8_t data, uint8_t sync = SYNC_ON_CLK);
uint8_t busRead(uint16_t addr, uint8_t sync = SYNC_ON_CLK);

void selectBank(uint16_t bank);
void selectRAMBank(uint8_t bank);
void enableRAM(bool enable);

void flashProgram(uint16_t addr, uint8_t data);
bool flashProgramVerify(uint16_t addr, uint8_t data);
bool flashVerify(uint16_t addr, uint8_t data);
bool waitForFlash(uint16_t addr, uint8_t expected, int timeout = MAX_FLASH_RETRIES);
bool flashEraseChip();
bool flashEraseSector(uint16_t sectorAddr);
void flashReadID(uint8_t &manufacturer, uint8_t &device);

bool programFile(const char *filename);
bool dumpFile(const char *filename);

bool blankRAM();
bool programRAMFile(const char *filename);
bool dumpRAMFile(const char *filename);

void setDataInput()
{
    curr_data = 0; // unknown until sampled
    for (uint8_t i = 0; i < 8; i++)
        pinMode(PIN_DATA[i], INPUT);
}

void setAddress(uint16_t addr)
{
    for(int i = 0; i < 16; i++)
        digitalWrite(PIN_ADDR[i], (addr >> i) & 1);
    curr_addr = addr;
}

uint16_t getAddress()
{
    curr_addr = 0;
    for(int i = 0; i < 16; i++)
        curr_addr |= (digitalRead(PIN_ADDR[i]) << i);
    return curr_addr;
}

void setData(uint8_t data)
{
    for(int i = 0; i < 8; i++) {
        pinMode(PIN_DATA[i], OUTPUT);
        digitalWrite(PIN_DATA[i], (data >> i) & 1);
    }
    curr_data = data;
}

uint8_t getData()
{
    curr_data = 0;
    for (uint8_t i = 0; i < 8; i++)
        curr_data |= (digitalRead(PIN_DATA[i]) << i);
    return curr_data;
}

void setBus(uint16_t address, uint8_t data)
{
    setAddress(address);
    setData(data);
}

void assertClock()
{
    digitalWrite(PIN_CLK, LOW);
}

void deassertClock()
{
    digitalWrite(PIN_CLK, HIGH);
}

void assertRead()
{
    digitalWrite(PIN_WR, HIGH);
    digitalWrite(PIN_RD, LOW);
}

void deassertRead()
{
    digitalWrite(PIN_RD, HIGH);
}

void assertWrite()
{
    digitalWrite(PIN_RD, HIGH);
    digitalWrite(PIN_WR, LOW);
}

void deassertWrite()
{
    digitalWrite(PIN_WR, HIGH);
}

void assertCableSelect()
{
    digitalWrite(PIN_CS, LOW);
}

void deassertCableSelect()
{
    digitalWrite(PIN_CS, HIGH);
}

void deassertAllControls()
{
    digitalWrite(PIN_CLK, HIGH);
    digitalWrite(PIN_WR, HIGH);
    digitalWrite(PIN_RD, HIGH);
    digitalWrite(PIN_CS, HIGH);
    delayMicroseconds(1);
}

void resetDevice()
{
    delayMicroseconds(1);
    digitalWrite(PIN_RST, LOW);
    rom_bank = 1;
    ram_bank = 0;
    ram_enabled = false;
    curr_addr = 0;
    curr_data = 0;
    delay(1);
    digitalWrite(PIN_RST, HIGH);
    delayMicroseconds(1);
}

uint32_t mapAddress(uint16_t addr)
{
    return addr < ROM_START + ROM_BANK_SIZE ? (addr - ROM_START)
           : (rom_bank * ROM_BANK_SIZE) + ((addr - ROM_START) % ROM_BANK_SIZE);
}

void demapAddress(uint32_t physAddr, uint8_t &bank, uint16_t &offset)
{
    bank = physAddr / ROM_BANK_SIZE;
    offset = physAddr % ROM_BANK_SIZE;
}

uint32_t mapRAMAddress(uint16_t addr)
{
    return addr >= RAM_START ? (ram_bank * RAM_BANK_SIZE) + ((addr - RAM_START) % RAM_BANK_SIZE)
                             : (ram_bank * RAM_BANK_SIZE) + (addr % RAM_BANK_SIZE);
}

void demapRAMAddress(uint32_t physAddr, uint8_t &bank, uint16_t &offset)
{
    bank = physAddr / RAM_BANK_SIZE;
    offset = physAddr % RAM_BANK_SIZE;
}

void busWrite(uint16_t addr, uint8_t data, uint8_t sync)
{
    if (sync & SYNC_ON_CLK) deassertClock();
    if (sync & SYNC_ON_CS) deassertCableSelect();
    deassertRead();
    deassertWrite();
    setDataInput();
    delayMicroseconds(1);

    setBus(addr, data); // forces data output mode

    if (sync & SYNC_ON_CLK) {
        if (sync & SYNC_ON_CS) assertCableSelect();
        assertWrite();
        assertClock();
    } else if (sync & SYNC_ON_WR) {
        if (sync & SYNC_ON_CS) assertCableSelect();
        assertWrite();
    } else if (sync & SYNC_ON_CS) {
        assertWrite();
        assertCableSelect();
    } else {
        assertWrite();
    }

    delayMicroseconds(1);
}

uint8_t busRead(uint16_t addr, uint8_t sync)
{
    if (sync & SYNC_ON_CLK) deassertClock();
    if (sync & SYNC_ON_CS) deassertCableSelect();
    deassertRead();
    deassertWrite();
    setDataInput();
    delayMicroseconds(1);

    setAddress(addr);

    if (sync & SYNC_ON_CLK) {
        if (sync & SYNC_ON_CS) assertCableSelect();
        assertRead();
        assertClock();
    } else if (sync & SYNC_ON_RD) {
        if (sync & SYNC_ON_CS) assertCableSelect();
        assertRead();
    } else if (sync & SYNC_ON_CS) {
        assertRead();
        assertCableSelect();
    } else {
        assertRead();
    }

    delayMicroseconds(1);

    uint8_t data = getData();
    delayMicroseconds(1);
    return data;
}

void selectBank(uint16_t bank)
{
    deassertAllControls(); // Safety

    rom_bank = bank ? bank : 1;

    busWrite(0x2000, (uint8_t)(rom_bank & 0x00FF));
    if (rom_bank > 0x00FF) // MBC5 support
        busWrite(0x3000, (uint8_t)((rom_bank & 0xFF00) >> 8));
}

void selectRAMBank(uint8_t bank)
{
    deassertAllControls(); // Safety

    busWrite(0x4000, (ram_bank = bank));
}

void enableRAM(bool enable)
{
    deassertAllControls(); // Safety

    busWrite(0x0000, (ram_enabled = enable) ? 0x0A : 0x00);
}

void flashProgram(uint16_t addr, uint8_t data)
{
    deassertAllControls(); // Safety

    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_UNLOCK_DATA_1, SYNC_ON_WR | SYNC_ON_CLK);
    busWrite(FLASH_UNLOCK_ADDR_2, FLASH_UNLOCK_DATA_2, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_PROGRAM_CMD, SYNC_ON_WR);
    busWrite(addr, data, SYNC_ON_WR);
}

bool flashProgramVerify(uint16_t addr, uint8_t data)
{
    flashProgram(addr, data);
    delayMicroseconds(50);
    return data == busRead(addr, SYNC_ON_RD);
}

bool flashVerify(uint16_t addr, uint8_t data)
{
    deassertAllControls(); // Safety

    return busRead(addr, SYNC_ON_RD) == data;
}

bool waitForFlash(uint16_t addr, uint8_t expected, int timeout = MAX_FLASH_RETRIES)
{
    while (timeout-- > 0) {
        // DQ7 check (final data bit stability)
        uint8_t now1 = busRead(addr, SYNC_ON_RD);
        if ((now1 & 0x80) == (expected & 0x80)) {
            // confirm stable twice
            uint8_t now2 = busRead(addr, SYNC_ON_RD);
            if ((now2 & 0x80) == (expected & 0x80))
                return true;
        }

        delay(1);
    }

    return false; // timeout
}

bool flashEraseChip()
{
    deassertAllControls(); // Safety

    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_UNLOCK_DATA_1, SYNC_ON_WR | SYNC_ON_CLK);
    busWrite(FLASH_UNLOCK_ADDR_2, FLASH_UNLOCK_DATA_2, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_ERASE_CMD, SYNC_ON_WR);

    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_UNLOCK_DATA_1, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_2, FLASH_UNLOCK_DATA_2, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_CHIP_ERASE_CMD, SYNC_ON_WR);

    // expected = 0xFF after erase
    if (!waitForFlash(0x0000, 0xFF)) {
        if (interactive_mode)
            Serial.println("FAIL CHIP ERASE TIMEOUT");
        return false;
    }

    return true;
}

bool flashEraseSector(uint16_t sectorAddr)
{
    deassertAllControls(); // Safety

    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_UNLOCK_DATA_1, SYNC_ON_WR | SYNC_ON_CLK);
    busWrite(FLASH_UNLOCK_ADDR_2, FLASH_UNLOCK_DATA_2, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_ERASE_CMD, SYNC_ON_WR);

    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_UNLOCK_DATA_1, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_2, FLASH_UNLOCK_DATA_2, SYNC_ON_WR);
    busWrite(sectorAddr, FLASH_SECT_ERASE_CMD, SYNC_ON_WR);

    // expected = 0xFF after erase
    if (!waitForFlash(sectorAddr, 0xFF)) {
        if (interactive_mode) {
            Serial.print("FAIL SECTOR ERASE TIMEOUT @ 0x");
            if (sectorAddr < 0x1000) Serial.print('0');
            if (sectorAddr < 0x0100) Serial.print('0');
            if (sectorAddr < 0x0010) Serial.print('0');
            Serial.println(sectorAddr);
        }
        return false;
    }

    return true;
}

void flashReadID(uint8_t &manufacturer, uint8_t &device)
{
    deassertAllControls(); // Safety

    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_UNLOCK_DATA_1, SYNC_ON_WR | SYNC_ON_CLK);
    busWrite(FLASH_UNLOCK_ADDR_2, FLASH_UNLOCK_DATA_2, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_ID_ENTRY_CMD, SYNC_ON_WR);

    manufacturer = busRead(0x0000, SYNC_ON_RD);
    device       = busRead(0x0001, SYNC_ON_RD);

    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_UNLOCK_DATA_1, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_2, FLASH_UNLOCK_DATA_2, SYNC_ON_WR);
    busWrite(FLASH_UNLOCK_ADDR_1, FLASH_ID_EXIT_CMD, SYNC_ON_WR);
}

inline void blinkForAddress(unsigned int addr)
{
    digitalWrite(PIN_LED, ADDR_BLINK_DIV ? (addr / ADDR_BLINK_DIV) % 2 ? HIGH : LOW : HIGH);
}

bool programFile(const char *filename)
{
    File rom = SD.open(filename, FILE_READ);
    if (!rom) {
        if (interactive_mode)
            Serial.println("SD OPEN FAILED");
        return false;
    }

    if (interactive_mode)
        Serial.println("PROGRAM ROM START");

    uint32_t romSize = rom.size();
    unsigned int errorCount = 0;
    uint16_t origBank = rom_bank;

    for (uint8_t phase = 0; phase < 2 && errorCount < MAX_ERROR_COUNT; ++phase) {
        for (uint32_t physAddr = 0; physAddr < romSize && errorCount < MAX_ERROR_COUNT; physAddr++) {
            uint8_t bank;
            uint16_t offset;
            demapAddress(physAddr, bank, offset);
            uint16_t addr = ROM_START + (bank ? ROM_BANK_SIZE : 0) + offset;

            if (offset == 0) {
                if (!phase && interactive_mode) {
                    Serial.print("Writing bank ");
                    Serial.println(bank);
                }
                selectBank(bank);
            }

            blinkForAddress(physAddr);

            if (!phase)
                flashProgram(addr, rom.read());
            else if (!flashVerify(addr, rom.read())) {
                if (interactive_mode) {
                    Serial.print("FAIL @ BYTE #");
                    Serial.print(physAddr);
                    Serial.print(", BANK: 0x");
                    if (bank < 0x1000) Serial.print('0');
                    if (bank < 0x0100) Serial.print('0');
                    if (bank < 0x0010) Serial.print('0');
                    Serial.print(bank, HEX);
                    Serial.print(", ADDR: 0x");
                    if (addr < 0x1000) Serial.print('0');
                    if (addr < 0x0100) Serial.print('0');
                    if (addr < 0x0010) Serial.print('0');
                    Serial.print(addr, HEX);
                    Serial.print(", PHYS_ADDR: 0x");
                    if (physAddr < 0x10000000) Serial.print('0');
                    if (physAddr < 0x01000000) Serial.print('0');
                    if (physAddr < 0x00100000) Serial.print('0');
                    if (physAddr < 0x00010000) Serial.print('0');
                    if (physAddr < 0x00001000) Serial.print('0');
                    if (physAddr < 0x00000100) Serial.print('0');
                    if (physAddr < 0x00000010) Serial.print('0');
                    Serial.println(physAddr, HEX);
                }
                errorCount++;
            }
        }

        if (!phase) {
            delayMicroseconds(50);
            rom.seek(0);
            if (interactive_mode)
                Serial.println("PROGRAM ROM VERIFY");
        }
    }

    rom.close();
    selectBank(origBank);
    digitalWrite(PIN_LED, LOW);

    if (interactive_mode)
        Serial.println("PROGRAM ROM DONE");
    return errorCount == 0;
}

bool dumpFile(const char *filename)
{
    File rom = SD.open(filename, FILE_WRITE);
    if (!rom) {
        if (interactive_mode)
            Serial.println("SD OPEN FAILED");
        return false;
    }

    if (interactive_mode)
        Serial.println("ROM DUMP START");

    uint16_t origBank = rom_bank;

    for (uint32_t physAddr = 0; physAddr < rom_size; physAddr++) {
        uint8_t bank;
        uint16_t offset;
        demapAddress(physAddr, bank, offset);
        uint16_t addr = ROM_START + (bank ? ROM_BANK_SIZE : 0) + offset;

        if (offset == 0) {
            if (interactive_mode) {
                Serial.print("Dumping bank ");
                Serial.println(bank);
            }
            selectBank(bank);
        }

        blinkForAddress(physAddr);

        rom.write(busRead(addr));
    }

    rom.close();
    selectBank(origBank);
    digitalWrite(PIN_LED, LOW);

    if (interactive_mode)
        Serial.println("ROM DUMP DONE");
    return true;
}

bool blankRAM()
{
    if (interactive_mode)
        Serial.println("BLANK RAM START");

    unsigned int errorCount = 0;
    uint16_t origBank = ram_bank;
    if (!ram_enabled)
        enableRAM(true);

    for (uint8_t phase = 0; phase < 2 && errorCount < MAX_ERROR_COUNT; ++phase) {
        for (uint32_t physAddr = 0; physAddr < ram_size && errorCount < MAX_ERROR_COUNT; ++physAddr) {
            uint8_t bank;
            uint16_t offset;
            demapRAMAddress(physAddr, bank, offset);
            uint16_t addr = RAM_START + offset;

            if (offset == 0) {
                if (!phase && interactive_mode) {
                    Serial.print("Resetting bank ");
                    Serial.println(bank);
                }
                selectRAMBank(bank);
            }

            blinkForAddress(physAddr);

            if (!phase)
                busWrite(addr, 0x00, SYNC_ON_CLK | SYNC_ON_CS);
            else if (busRead(addr, SYNC_ON_CLK | SYNC_ON_CS) != 0x00) {
                if (interactive_mode) {
                    Serial.print("FAIL @ BYTE #");
                    Serial.print(physAddr);
                    Serial.print(", BANK: 0x");
                    if (bank < 0x1000) Serial.print('0');
                    if (bank < 0x0100) Serial.print('0');
                    if (bank < 0x0010) Serial.print('0');
                    Serial.print(bank, HEX);
                    Serial.print(", ADDR: 0x");
                    if (addr < 0x1000) Serial.print('0');
                    if (addr < 0x0100) Serial.print('0');
                    if (addr < 0x0010) Serial.print('0');
                    Serial.print(addr, HEX);
                    Serial.print(", PHYS_ADDR: 0x");
                    if (physAddr < 0x10000000) Serial.print('0');
                    if (physAddr < 0x01000000) Serial.print('0');
                    if (physAddr < 0x00100000) Serial.print('0');
                    if (physAddr < 0x00010000) Serial.print('0');
                    if (physAddr < 0x00001000) Serial.print('0');
                    if (physAddr < 0x00000100) Serial.print('0');
                    if (physAddr < 0x00000010) Serial.print('0');
                    Serial.println(physAddr, HEX);
                }
                errorCount++;
            }
        }

        if (!phase) {
            delayMicroseconds(5);
            if (interactive_mode)
                Serial.println("BLANK RAM VERIFY");
        }
    }

    selectRAMBank(origBank);
    digitalWrite(PIN_LED, LOW);

    if (interactive_mode)
        Serial.println("BLANK RAM DONE");
    return errorCount == 0;
}

bool programRAMFile(const char *filename)
{
    File ram = SD.open(filename, FILE_READ);
    if (!ram) {
        if (interactive_mode)
            Serial.println("SD OPEN FAILED");
        return false;
    }

    if (interactive_mode)
        Serial.println("PROGRAM RAM START");

    uint32_t ramSize = ram.size();
    unsigned int errorCount = 0;
    uint16_t origBank = ram_bank;
    if (!ram_enabled)
        enableRAM(true);

    for (uint8_t phase = 0; phase < 2 && errorCount < MAX_ERROR_COUNT; ++phase) {
        for (uint32_t physAddr = 0; physAddr < ramSize && errorCount < MAX_ERROR_COUNT; physAddr++) {
            uint8_t bank;
            uint16_t offset;
            demapRAMAddress(physAddr, bank, offset);
            uint16_t addr = RAM_START + offset;
    
            if (offset == 0) {
                if (!phase && interactive_mode) {
                    Serial.print("Writing bank ");
                    Serial.println(bank);
                }
                selectRAMBank(bank);
            }
    
            blinkForAddress(physAddr);
    
            if (!phase)
                busWrite(addr, ram.read(), SYNC_ON_CLK | SYNC_ON_CS);
            else if (busRead(addr, SYNC_ON_CLK | SYNC_ON_CS) != ram.read()) {
                if (interactive_mode) {
                    Serial.print("FAIL @ BYTE #");
                    Serial.print(physAddr);
                    Serial.print(", BANK: 0x");
                    if (bank < 0x1000) Serial.print('0');
                    if (bank < 0x0100) Serial.print('0');
                    if (bank < 0x0010) Serial.print('0');
                    Serial.print(bank, HEX);
                    Serial.print(", ADDR: 0x");
                    if (addr < 0x1000) Serial.print('0');
                    if (addr < 0x0100) Serial.print('0');
                    if (addr < 0x0010) Serial.print('0');
                    Serial.print(addr, HEX);
                    Serial.print(", PHYS_ADDR: 0x");
                    if (physAddr < 0x10000000) Serial.print('0');
                    if (physAddr < 0x01000000) Serial.print('0');
                    if (physAddr < 0x00100000) Serial.print('0');
                    if (physAddr < 0x00010000) Serial.print('0');
                    if (physAddr < 0x00001000) Serial.print('0');
                    if (physAddr < 0x00000100) Serial.print('0');
                    if (physAddr < 0x00000010) Serial.print('0');
                    Serial.println(physAddr, HEX);
                    errorCount++;
                }
            }
        }

        if (!phase) {
            delayMicroseconds(5);
            ram.seek(0);
            if (interactive_mode)
                Serial.println("PROGRAM RAM VERIFY");
        }
    }

    ram.close();
    selectRAMBank(origBank);
    digitalWrite(PIN_LED, LOW);

    if (interactive_mode)
        Serial.println("PROGRAM RAM DONE");
    return errorCount == 0;
}

bool dumpRAMFile(const char *filename)
{
    File ram = SD.open(filename, FILE_WRITE);
    if (!ram) {
        if (interactive_mode)
            Serial.println("SD OPEN FAILED");
        return false;
    }

    if (interactive_mode)
        Serial.println("RAM DUMP START");

    uint16_t origBank = ram_bank;
    if (!ram_enabled)
        enableRAM(true);

    for (uint32_t physAddr = 0; physAddr < ram_size; physAddr++) {
        uint8_t bank;
        uint16_t offset;
        demapRAMAddress(physAddr, bank, offset);
        uint16_t addr = RAM_START + offset;

        if (offset == 0) {
            if (interactive_mode) {
                Serial.print("Dumping bank ");
                Serial.println(bank);
            }
            selectRAMBank(bank);
        }

        blinkForAddress(physAddr);

        ram.write(busRead(addr, SYNC_ON_CLK | SYNC_ON_CS));
    }

    ram.close();
    selectRAMBank(origBank);
    digitalWrite(PIN_LED, LOW);

    if (interactive_mode)
        Serial.println("RAM DUMP DONE");
    return true;
}


// -----------------------------------------------------------------------------
// Command Interpreter
// -----------------------------------------------------------------------------

void serialTask();
void processCommand(String line);

void cmdHelp();
void cmdStatus();
void cmdReset();

void cmdClock(bool state);
void cmdRead(bool state);
void cmdWrite(bool state);
void cmdCS(bool state);
void cmdDeassert();

void cmdAddress();
void cmdAddress(uint16_t addr);
void cmdData();
void cmdData(uint8_t data);

void cmdBusRead(uint16_t addr);
void cmdBusWrite(uint16_t addr, uint8_t data);

void cmdBank(uint16_t bank);
void cmdSize(uint32_t size);
void cmdFlashID();
void cmdEraseChip();
void cmdEraseSector(uint16_t addr);
void cmdProgram(uint16_t addr, uint8_t data);
void cmdProgram(const char *filename);
void cmdDump(const char *filename);

void cmdRAM(bool enable);
void cmdRAMBank(uint8_t bank);
void cmdRAMSize(uint32_t size);
void cmdRAMBlank();
void cmdRAMProgram(const char *filename);
void cmdRAMDump(const char *filename);

void serialTask()
{
    static String line;

    if (!interactive_mode)
        return;

    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\r')
            continue;

        if (c == '\n') {
            line.trim();

            if (line.length())
                processCommand(line);

            line = "";
        } else {
            line += c;
        }
    }
}

void processCommand(String line)
{
    line.trim();
    line.toUpperCase();

    char buffer[80] = { '\0' };
    line.toCharArray(buffer, sizeof(buffer));

    char *argv[8] = { nullptr };
    int argc = 0;
    char *tok = strtok(buffer, " ");

    while (tok && argc < 8) {
        argv[argc++] = tok;
        tok = strtok(nullptr, " ");
    }

    if (!argc) return;

    //--------------------------------------------------
    // HELP
    //--------------------------------------------------
    if (!strcmp(argv[0], "HELP"))
    {
        cmdHelp();
        return;
    }

    //--------------------------------------------------
    // STATUS
    //--------------------------------------------------
    if (!strcmp(argv[0], "STATUS"))
    {
        cmdStatus();
        return;
    }

    //--------------------------------------------------
    // RESET
    //--------------------------------------------------
    if (!strcmp(argv[0], "RESET"))
    {
        cmdReset();
        return;
    }

    //--------------------------------------------------
    // CLK <0|1>
    //--------------------------------------------------
    if (!strcmp(argv[0], "CLK") && argc == 2)
    {
        cmdClock(strtoul(argv[1], nullptr, 16));
        return;
    }

    //--------------------------------------------------
    // RD <0|1>
    //--------------------------------------------------
    if (!strcmp(argv[0], "RD") && argc == 2)
    {
        cmdRead(strtoul(argv[1], nullptr, 16));
        return;
    }
    
    //--------------------------------------------------
    // WR <0|1>
    //--------------------------------------------------
    if (!strcmp(argv[0], "WR") && argc == 2)
    {
        cmdWrite(strtoul(argv[1], nullptr, 16));
        return;
    }
    
    //--------------------------------------------------
    // CS <0|1>
    //--------------------------------------------------
    if (!strcmp(argv[0], "CS") && argc == 2)
    {
        cmdCS(strtoul(argv[1], nullptr, 16));
        return;
    }

    //--------------------------------------------------
    // DEASSERT
    //--------------------------------------------------
    if (!strcmp(argv[0], "DEASSERT"))
    {
        cmdDeassert();
        return;
    }

    //--------------------------------------------------
    // ADDR
    // ADDR addr
    //--------------------------------------------------
    if (!strcmp(argv[0], "ADDR"))
    {
        if (argc == 1) {
            cmdAddress();
            return;
        } else if (argc == 2) {
            cmdAddress(strtoul(argv[1], nullptr, 16));
            return;
        }
    }

    //--------------------------------------------------
    // DATA
    // DATA data
    //--------------------------------------------------
    if (!strcmp(argv[0], "DATA"))
    {
        if (argc == 1) {
            cmdData();
            return;
        } else if (argc == 2) {
            cmdData(strtoul(argv[1], nullptr, 16));
            return;
        }
    }

    //--------------------------------------------------
    // READ addr
    //--------------------------------------------------
    if (!strcmp(argv[0], "READ") && argc == 2)
    {
        cmdBusRead(strtoul(argv[1], nullptr, 16));
        return;
    }

    //--------------------------------------------------
    // WRITE addr data
    //--------------------------------------------------
    if (!strcmp(argv[0], "WRITE") && argc == 3)
    {
        cmdBusWrite(strtoul(argv[1], nullptr, 16),
                    strtoul(argv[2], nullptr, 16));
        return;
    }

    //--------------------------------------------------
    // BANK xxxx
    //--------------------------------------------------
    if (!strcmp(argv[0], "BANK") && argc == 2)
    {
        cmdBank(strtoul(argv[1], nullptr, 16));
        return;
    }

    //--------------------------------------------------
    // SIZE d
    //--------------------------------------------------
    if (!strcmp(argv[0], "SIZE") && argc == 2)
    {
        cmdSize(strtoul(argv[1], nullptr, 0));
        return;
    }

    //--------------------------------------------------
    // ID
    //--------------------------------------------------
    if (!strcmp(argv[0], "ID"))
    {
        cmdFlashID();
        return;
    }

    //--------------------------------------------------
    // ERASE CHIP
    //--------------------------------------------------
    if (!strcmp(argv[0], "ERASE") && argc == 2 && !strcmp(argv[1], "CHIP"))
    {
        cmdEraseChip();
        return;
    }

    //--------------------------------------------------
    // ERASE addr
    //--------------------------------------------------
    if (!strcmp(argv[0], "ERASE") && argc == 2)
    {
        cmdEraseSector(strtoul(argv[1], nullptr, 16));
        return;
    }

    //--------------------------------------------------
    // PROG filename
    // PROG addr data
    //--------------------------------------------------
    if (!strcmp(argv[0], "PROG") || !strcmp(argv[0], "PROGRAM"))
    {
        if (argc == 2) {
            cmdProgram(argv[1]);
            return;
        } else if (argc == 3) {
            uint16_t addr = (uint16_t)strtoul(argv[1], nullptr, 16);
            uint8_t data = (uint8_t)strtoul(argv[2], nullptr, 16);
            cmdProgram(addr, data);
            return;
        }
    }

    //--------------------------------------------------
    // DUMP filename
    //--------------------------------------------------
    if (!strcmp(argv[0], "DUMP") && argc == 2)
    {
        cmdDump(argv[1]);
        return;
    }

    //--------------------------------------------------
    // RAM <ON|OFF>
    //--------------------------------------------------
    if (!strcmp(argv[0], "RAM"))
    {
        bool enable = false;

        if (!strcmp(argv[1], "ON") || !strcmp(argv[1], "1") || !strcmp(argv[1], "ENABLE"))
            enable = true;
        else if (!strcmp(argv[1], "OFF") || !strcmp(argv[1], "0") || !strcmp(argv[1], "DISABLE"))
            enable = false;
        else
        {
            Serial.println();
            Serial.println("RAM expects ON/OFF");
            return;
        }

        cmdRAM(enable);
        return;
    }

    //--------------------------------------------------
    // RAMBANK xx
    //--------------------------------------------------
    if (!strcmp(argv[0], "RAMBANK") && argc == 2)
    {
        cmdRAMBank((uint8_t)strtoul(argv[1], nullptr, 16));
        return;
    }

    //--------------------------------------------------
    // RAMSIZE d
    //--------------------------------------------------
    if (!strcmp(argv[0], "RAMSIZE") && argc == 2)
    {
        cmdRAMSize(strtoul(argv[1], nullptr, 0));
        return;
    }

    //--------------------------------------------------
    // RAMBLANK
    //--------------------------------------------------
    if (!strcmp(argv[0], "RAMBLANK") || !strcmp(argv[0], "RAMERASE"))
    {
        cmdRAMBlank();
        return;
    }

    //--------------------------------------------------
    // RAMPROG filename
    //--------------------------------------------------
    if ((!strcmp(argv[0], "RAMPROG") || !strcmp(argv[0], "RAMPROGRAM")) && argc == 2)
    {
        cmdRAMProgram(argv[1]);
        return;
    }

    //--------------------------------------------------
    // RAMDUMP filename
    //--------------------------------------------------
    if (!strcmp(argv[0], "RAMDUMP") && argc == 2)
    {
        cmdRAMDump(argv[1]);
        return;
    }

    Serial.println("Unknown command.");
}

void cmdHelp()
{
    Serial.println();
    Serial.println("=== HELP ===");
    Serial.println("Basic Commands:");
    Serial.println("HELP                  -> This help menu");
    Serial.println("STATUS                -> Status readout (/w full refresh)");
    Serial.println("RESET                 -> Device reset (done automatically on startup)");
    Serial.println();
    Serial.println("Pin Commands:");
    Serial.println("CLK <0|1>             -> Asserts/deasserts CLK (asserts/syncs on falling edge)");
    Serial.println("WR <0|1>              -> Asserts/deasserts /WE");
    Serial.println("RD <0|1>              -> Asserts/deasserts /RD");
    Serial.println("CS <0|1>              -> Asserts/deasserts /CS (only for RAM enable override)");
    Serial.println("DEASSERT              -> Deasserts all control lines");
    Serial.println("ADDR                  -> Displays current ADDR (/w refresh)");
    Serial.println("ADDR <addr>           -> Sets address to ADDR");
    Serial.println("DATA                  -> Displays current DATA (/w refresh)");
    Serial.println("DATA <data>           -> Sets data to DATA (forces output mode - may contend)");
    Serial.println();
    Serial.println("Bus Commands:");
    Serial.println("READ  <addr>          -> Reads from ADDR");
    Serial.println("WRITE <addr> <data>   -> Writes DATA to ADDR");
    Serial.println();
    Serial.println("ROM Commands:");
    Serial.println("BANK <hex>            -> Sets ROM bank # (>0xFF req. MBC5 compat.)");
    Serial.println("SIZE <bytes>          -> Sets size of ROM for dumps (default: 4MB)");
    Serial.println("ID                    -> Displays ROM Manufacturer/Device ID");
    Serial.println("ERASE CHIP            -> Erases entire ROM chip to 0xFF");
    Serial.println("ERASE <sector>        -> Erases specific ROM sector to 0xFF");
    Serial.println("PROG <addr> <data>    -> Programs DATA to ADDR");
    Serial.println("PROG <file>           -> Programs ROM from SD file");
    Serial.println("DUMP <file>           -> Dumps ROM to SD file (requires SIZE set)");
    Serial.println();
    Serial.println("RAM Commands:");
    Serial.println("RAM <ON|OFF>          -> Sets RAM on/off");
    Serial.println("RAMBANK <hex>         -> Sets RAM bank #");
    Serial.println("RAMSIZE <bytes>       -> Sets size of RAM for dumps/blanking (default: 32KB)");
    Serial.println("RAMBLANK              -> Resets entire RAM chip to 0x00 (requires RAMSIZE set)");
    Serial.println("RAMPROG <file>        -> Programs RAM from SD file");
    Serial.println("RAMDUMP <file>        -> Dumps RAM to SD file (requires RAMSIZE set)");
    Serial.println();
    Serial.println("Address Map:");
    Serial.println("     ROM BANK 0:      0x0000-0x3FFF (locked to bank 0)");
    Serial.println("   ROM BANK 1/X:      0x4000-0x7FFF (settable bank)");
    Serial.println("            RAM:      0xA000-0xBFFF (settable bank)");
    Serial.println();
    Serial.println("LED Status:");
    Serial.println("       Solid On:      Programming \"/rom.bin\" completed (standalone mode)");
    Serial.println("     Fast Blink:      Failure programming \"/rom.bin\" (standalone mode)");
    Serial.println("Very Fast Blink:      Programming/erasing chip activity");
    Serial.println("     Slow Blink:      Ready for commands (interactive mode)");
    Serial.println("      Solid Off:      Not yet initialized / hardware fault");
    Serial.println();
    Serial.println("ROM Short-Hand Sizes: 512,1,2,4,8,16,32,64      (512Kbit to 64Mbit)");
    Serial.println("RAM Short-Hand Sizes: 2,4,8,16,32,64,128,256,512,1 (2Kbit to 1Mbit)");
}

inline bool isOutput(uint8_t pin)
{
    return (*portModeRegister(digitalPinToPort(pin)) &
            digitalPinToBitMask(pin)) != 0;
}

void cmdStatus()
{
    Serial.println();
    Serial.println("=== Status ===");

    Serial.print("ADDR    : 0x");
    getAddress();
    if (curr_addr < 0x1000) Serial.print('0');
    if (curr_addr < 0x0100) Serial.print('0');
    if (curr_addr < 0x0010) Serial.print('0');
    Serial.println(curr_addr, HEX);

    Serial.print("DATA    : 0x");
    getData();
    if (curr_data < 0x10) Serial.print('0');
    Serial.print(curr_data, HEX);
    if (isOutput(PIN_DATA[0])) {
        Serial.print(" (driving");
        if (digitalRead(PIN_WR))
            Serial.println(", /WR not asserted => data bus possibly under contention)");
        else
            Serial.println(")");
    } else
        Serial.println(" (not-driving)");

    Serial.print("CLK     : ");
    Serial.println(digitalRead(PIN_CLK) ? "HIGH" : "LOW");

    Serial.print("/WR     : ");
    Serial.println(digitalRead(PIN_WR) ? "HIGH" : "LOW");

    Serial.print("/RD     : ");
    Serial.println(digitalRead(PIN_RD) ? "HIGH" : "LOW");

    Serial.print("/CS     : ");
    Serial.println(digitalRead(PIN_CS) ? "HIGH" : "LOW");

    Serial.print("/RST    : ");
    Serial.println(digitalRead(PIN_RST) ? "HIGH" : "LOW");

    Serial.print("ROM BANK: 0x");
    if (rom_bank < 0x1000) Serial.print('0');
    if (rom_bank < 0x0100) Serial.print('0');
    if (rom_bank < 0x0010) Serial.print('0');
    Serial.println(rom_bank, HEX);

    Serial.print("ROM SIZE: ");
    Serial.println(rom_size);

    Serial.print("RAM     : ");
    Serial.println(ram_enabled ? "ENABLED" : "DISABLED");

    Serial.print("RAM BANK: 0x");
    if (ram_bank < 0x10) Serial.print('0');
    Serial.println(ram_bank, HEX);

    Serial.print("RAM SIZE: ");
    Serial.println(ram_size);

    Serial.println();
}

void cmdReset()
{
    Serial.println();
    Serial.println("Resetting device");
    resetDevice();
    Serial.println("OK");
}

void cmdClock(bool state)
{
    Serial.println();

    if (state)
        deassertClock();
    else
        assertClock();

    Serial.print("CLK <= ");
    Serial.println(state ? "HIGH" : "LOW");
}

void cmdRead(bool state)
{
    Serial.println();

    if (state)
        deassertRead();
    else
        assertRead();

    Serial.print("/RD <= ");
    Serial.println(state ? "HIGH" : "LOW");
}

void cmdWrite(bool state)
{
    Serial.println();

    if (state)
        deassertWrite();
    else
        assertWrite();

    Serial.print("/WR <= ");
    Serial.println(state ? "HIGH" : "LOW");
}

void cmdCS(bool state)
{
    Serial.println();

    if (state)
        deassertCableSelect();
    else
        assertCableSelect();

    Serial.print("/CS <= ");
    Serial.println(state ? "HIGH" : "LOW");
}

void cmdDeassert()
{
    Serial.println();

    deassertAllControls();

    Serial.println("CLK <= HIGH");
    Serial.println("/WR <= HIGH");
    Serial.println("/RD <= HIGH");
    Serial.println("/CS <= HIGH");
}

void cmdAddress()
{
    Serial.println();

    getAddress();

    Serial.print("ADDR => 0x");
    if (curr_addr < 0x1000) Serial.print('0');
    if (curr_addr < 0x0100) Serial.print('0');
    if (curr_addr < 0x0010) Serial.print('0');
    Serial.println(curr_addr, HEX);
}

void cmdAddress(uint16_t addr)
{
    Serial.println();

    setAddress(addr);

    Serial.print("ADDR <= 0x");
    if (curr_addr < 0x1000) Serial.print('0');
    if (curr_addr < 0x0100) Serial.print('0');
    if (curr_addr < 0x0010) Serial.print('0');
    Serial.println(curr_addr, HEX);
}

void cmdData()
{
    Serial.println();

    getData();

    Serial.print("DATA => 0x");
    if (curr_data < 0x10) Serial.print('0');
    Serial.println(curr_data, HEX);
}

void cmdData(uint8_t data)
{
    Serial.println();

    setData(data);

    Serial.print("DATA <= 0x");
    if (data < 0x10) Serial.print('0');
    Serial.println(data, HEX);
}

void cmdBusRead(uint16_t addr)
{
    Serial.println();

    uint8_t data = busRead(addr);

    Serial.print("0x");
    if (addr < 0x1000) Serial.print('0');
    if (addr < 0x0100) Serial.print('0');
    if (addr < 0x0010) Serial.print('0');
    Serial.print(addr, HEX);
    Serial.print(" => 0x");
    if (data < 0x10) Serial.print('0');
    Serial.println(data, HEX);
}

void cmdBusWrite(uint16_t addr, uint8_t data)
{
    Serial.println();

    busWrite(addr, data);

    Serial.print("0x");
    if (addr < 0x1000) Serial.print('0');
    if (addr < 0x0100) Serial.print('0');
    if (addr < 0x0010) Serial.print('0');
    Serial.print(addr, HEX);
    Serial.print(" <= 0x");
    if (data < 0x10) Serial.print('0');
    Serial.println(data, HEX);
}

void cmdBank(uint16_t bank)
{
    Serial.println();

    selectBank(bank);

    Serial.print("ROM BANK <= 0x");
    if (rom_bank < 0x1000) Serial.print('0');
    if (rom_bank < 0x0100) Serial.print('0');
    if (rom_bank < 0x0010) Serial.print('0');
    Serial.println(rom_bank, HEX);
}

void cmdSize(uint32_t size)
{
    Serial.println();

    switch (size)
    {
        case ROM_SIZE_256KBIT: rom_size = 32UL * 1024UL;    break; // 256 Kbit
        case ROM_SIZE_512KBIT: rom_size = 64UL * 1024UL;    break; // 512 Kbit
        case ROM_SIZE_1MBIT:   rom_size = 128UL * 1024UL;   break; // 1 Mbit
        case ROM_SIZE_2MBIT:   rom_size = 256UL * 1024UL;   break; // 2 Mbit
        case ROM_SIZE_4MBIT:   rom_size = 512UL * 1024UL;   break; // 4 Mbit
        case ROM_SIZE_8MBIT:   rom_size = 1024UL * 1024UL;  break; // 8 Mbit
        case ROM_SIZE_16MBIT:  rom_size = 2048UL * 1024UL;  break; // 16 Mbit
        case ROM_SIZE_32MBIT:  rom_size = 4096UL * 1024UL;  break; // 32 Mbit
        case ROM_SIZE_64MBIT:  rom_size = 8192UL * 1024UL;  break; // 64 Mbit
        case ROM_SIZE_128MBIT: rom_size = 16384UL * 1024UL; break; // 128 Mbit
        default:               rom_size = size;             break; // Custom 
    }

    Serial.print("ROM SIZE <= ");
    Serial.println(rom_size);
}

void cmdFlashID()
{
    Serial.println();
    Serial.println("Reading device ID");

    uint8_t manufacturer = 0, device = 0;
    flashReadID(manufacturer, device);

    Serial.print("Manufacturer: 0x");
    if (manufacturer < 0x10) Serial.print('0');
    Serial.print(manufacturer, HEX);
    Serial.print(", Device: 0x");
    if (device < 0x10) Serial.print('0');
    Serial.println(device, HEX);
}

void cmdEraseChip()
{
    Serial.println();
    Serial.println("Erasing chip");

    if (flashEraseChip())
        Serial.println("OK");
    else
        Serial.println("FAILED");
}

void cmdEraseSector(uint16_t addr)
{
    Serial.println();
    Serial.print("Erasing sector 0x");
    if (addr < 0x1000) Serial.print('0');
    if (addr < 0x0100) Serial.print('0');
    if (addr < 0x0010) Serial.print('0');
    Serial.println(addr, HEX);

    if (flashEraseSector(addr))
        Serial.println("OK");
    else
        Serial.println("FAILED");
}

void cmdProgram(uint16_t addr, uint8_t data)
{
    Serial.println();
    Serial.print("Programming 0x");
    if (addr < 0x1000) Serial.print('0');
    if (addr < 0x0100) Serial.print('0');
    if (addr < 0x0010) Serial.print('0');
    Serial.print(addr, HEX);
    Serial.print(" <= 0x");
    if (data < 0x10) Serial.print('0');
    Serial.println(data, HEX);

    if (flashProgramVerify(addr, data))
        Serial.println("OK");
    else
        Serial.println("FAILED");
}

void cmdProgram(const char *filename)
{
    Serial.println();
    Serial.print("Programming ROM from file: ");
    Serial.println(filename);

    if (programFile(filename))
        Serial.println("OK");
    else
        Serial.println("FAILED");
}

void cmdDump(const char *filename)
{
    Serial.println();
    Serial.print("Dumping ROM to file: ");
    Serial.println(filename);

    if (dumpFile(filename))
        Serial.println("OK");
    else
        Serial.println("FAILED");
}

void cmdRAM(bool enable)
{
    Serial.println();

    enableRAM(enable);

    Serial.println(ram_enabled ? "RAM ENABLED" : "RAM DISABLED");
}

void cmdRAMBank(uint8_t bank)
{
    Serial.println();

    selectRAMBank(bank);

    Serial.print("RAM BANK <= 0x");
    if (ram_bank < 0x10) Serial.print('0');
    Serial.println(ram_bank, HEX);
}

void cmdRAMSize(uint32_t size)
{
    Serial.println();

    switch (size)
    {
        case RAM_SIZE_2KBIT:   ram_size = 256UL;          break; // 2 Kbit
        case RAM_SIZE_4KBIT:   ram_size = 512UL;          break; // 4 Kbit
        case RAM_SIZE_8KBIT:   ram_size = 1UL * 1024UL;   break; // 8 Kbit
        case RAM_SIZE_16KBIT:  ram_size = 2UL * 1024UL;   break; // 16 Kbit
        case RAM_SIZE_32KBIT:  ram_size = 4UL * 1024UL;   break; // 32 Kbit
        case RAM_SIZE_64KBIT:  ram_size = 8UL * 1024UL;   break; // 64 Kbit
        case RAM_SIZE_128KBIT: ram_size = 16UL * 1024UL;  break; // 128 Kbit
        case RAM_SIZE_256KBIT: ram_size = 32UL * 1024UL;  break; // 256 Kbit
        case RAM_SIZE_512KBIT: ram_size = 64UL * 1024UL;  break; // 512 Kbit
        case RAM_SIZE_1MBIT:   ram_size = 128UL * 1024UL; break; // 1 Mbit
        default:               ram_size = size;           break; // Custom 
    }

    Serial.print("RAM SIZE <= ");
    Serial.println(ram_size);
}

void cmdRAMBlank()
{
    Serial.println();
    Serial.println("Blanking RAM chip");

    if (blankRAM())
        Serial.println("OK");
    else
        Serial.println("FAILED");
}

void cmdRAMProgram(const char *filename)
{
    Serial.println();
    Serial.print("Programming RAM from file: ");
    Serial.println(filename);

    if (programRAMFile(filename))
        Serial.println("OK");
    else
        Serial.println("FAILED");
}

void cmdRAMDump(const char *filename)
{
    Serial.println();
    Serial.print("Dumping RAM to file: ");
    Serial.println(filename);

    if (dumpRAMFile(filename))
        Serial.println("OK");
    else
        Serial.println("FAILED");
}


// -----------------------------------------------------------------------------
// Main Program
// -----------------------------------------------------------------------------

void setup()
{
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    for (uint8_t i = 0; i < 16; i++) {
        pinMode(PIN_ADDR[i], OUTPUT);
        digitalWrite(PIN_ADDR[i], LOW);
    }

    pinMode(PIN_CLK, OUTPUT);
    digitalWrite(PIN_CLK, HIGH);
    pinMode(PIN_WR, OUTPUT);
    digitalWrite(PIN_WR, HIGH);
    pinMode(PIN_RD, OUTPUT);
    digitalWrite(PIN_RD, HIGH);
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, HIGH);

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    Serial.begin(115200);
    if (!Serial) { // standalone mode
        interactive_mode = false;

        SD.begin(PIN_SD_CS);

        resetDevice();
    } else {
        interactive_mode = true;

        Serial.println();
        Serial.println("Initializing SD card");

        if (!SD.begin(PIN_SD_CS))
            Serial.println("SD INIT FAILED");
        else {
            File root = SD.open("/");
            if (!root)
                Serial.println("SD ROOT FAILED");
            else
                Serial.println("SD ROOT MOUNTED");
            root.close();
        }

        cmdReset();

        Serial.println();
        Serial.println("READY - type HELP for help menu");
    }
}

void loop()
{
    static unsigned int blinkDiv = 1000;

    if (interactive_mode)
        serialTask();
    else if (!looped_once)
        blinkDiv = programFile("rom.bin") ? 0 : 100;

    digitalWrite(PIN_LED, blinkDiv ? ((millis() / blinkDiv) % 2) ? HIGH : LOW : HIGH);
    looped_once = true;
}
