/*-----------------------------------------------------------------------------*
 * extEEPROM.cpp - Arduino library to support external I2C EEPROMs.            *
 *                                                                             *
 * This library will work with most I2C serial EEPROM chips between 2k bits    *
 * and 2048k bits (2M bits) in size. Multiple EEPROMs on the bus are supported *
 * as a single address space. Certain assumptions are made regarding the       *
 * EEPROM device addressing. These assumptions should be true for most EEPROMs *
 * but there are exceptions, so read the datasheet and know your hardware.     *
 *                                                                             *
 * The library should also work for EEPROMs smaller than 2k bits, assuming     *
 * that there is only one on the bus and also that the user is careful not     *
 * to exceed the maximum address for the EEPROM.                               *
 *                                                                             *
 * Device addressing assumptions:                                              *
 * 1. The I2C address sequence consists of a control byte followed by one      *
 *    address byte (for EEPROMs <= 16k bits) or two address bytes (for         *
 *    EEPROMs > 16k bits).                                                     *
 * 2. The three least-significant bits in the control byte (excluding the R/W  *
 *    bit) comprise the three most-significant bits for the entire address     *
 *    space, i.e. all chips on the bus. As such, these may be chip-select      *
 *    bits or block-select bits (for individual chips that have an internal    *
 *    block organization), or a combination of both (in which case the         *
 *    block-select bits must be of lesser significance than the chip-select    *
 *    bits).                                                                   *
 * 3. Regardless of the number of bits needed to address the entire address    *
 *    space, the three most-significant bits always go in the control byte.    *
 *    Depending on EEPROM device size, this may result in one or more of the   *
 *    most significant bits in the I2C address bytes being unused (or "don't   *
 *    care").                                                                  *
 *                                                                             *
 * To use the extEEPROM library, the Arduino Wire library must also            *
 * be included.                                                                *
 *                                                                             *
 * Jack Christensen 23Mar2013 v1                                               *
 * 29Mar2013 v2 - Updated to span page boundaries (and therefore also          *
 * device boundaries, assuming an integral number of pages per device)         *
 * 08Jul2014 v3 - Generalized for 2kb - 2Mb EEPROMs.                           *
 *                                                                             *
 * External EEPROM Library by Jack Christensen is licensed under CC BY-SA 4.0, *
 * http://creativecommons.org/licenses/by-sa/4.0/                              *
 *-----------------------------------------------------------------------------*/

#include <extEEPROM.h>
#include <Wire.h>

// Constructor.
// - deviceCapacity is the capacity of a single EEPROM device in
//   kilobits (kb) and should be one of the values defined in the
//   eeprom_size_t enumeration in the extEEPROM.h file. (Most
//   EEPROM manufacturers use kbits in their part numbers.)
// - nDevice is the number of EEPROM devices on the I2C bus (all must
//   be identical).
// - pageSize is the EEPROM's page size in bytes.
extEEPROM::extEEPROM(eeprom_size_t deviceCapacity, byte nDevice, unsigned int pageSize)
{
    _dvcCapacity = deviceCapacity;
    _nDevice = nDevice;
    _pageSize = pageSize;
    _totalCapacity = _nDevice * _dvcCapacity * 1024UL / 8;
    _nAddrBytes = deviceCapacity > kbits_16 ? 2 : 1;       //two address bytes needed for eeproms > 16kbits

    //determine the bitshift needed to isolate the chip select bits from the address to put into the control byte
    uint16_t kb = _dvcCapacity;
    if ( kb <= kbits_16 ) _csShift = 8;
    else if ( kb >= kbits_512 ) _csShift = 16;
    else {
        kb >>= 6;
        _csShift = 12;
        while ( kb >= 1 ) {
            ++_csShift;
            kb >>= 1;
        }
    }
}

//initialize the I2C bus and try a dummy write (no data sent)
//to the device so that the caller can determine whether it is responding.
byte extEEPROM::begin(uint8_t eepromAddr)
{
    _eepromAddr = eepromAddr;
    Wire.begin(_eepromAddr);
    Wire.beginTransmission(_eepromAddr);
    if (_nAddrBytes == 2) i2cWrite(0);      //high addr byte
    i2cWrite(0);                            //low addr byte
    return Wire.endTransmission();
}

// Write bytes to external EEPROM.
// If the I/O would extend past the top of the EEPROM address space,
// a status of EEPROM_ADDR_ERR is returned. For I2C errors, the status
// from the Arduino Wire library is passed back through to the caller.
byte extEEPROM::write(unsigned long addr, byte *values, byte nBytes)
{
    uint8_t ctrlByte;       //control byte (I2C device address & chip/block select bits)
    uint8_t txStatus = 0;   //transmit status
    uint16_t nWrite;        //number of bytes to write
    uint16_t nPage;         //number of bytes remaining on current page, starting at addr

    if (addr + nBytes > _totalCapacity) {   //will this write go past the top of the EEPROM?
        return EEPROM_ADDR_ERR;             //yes, tell the caller
    }

    while (nBytes > 0) {
        nPage = _pageSize - ( addr & (_pageSize - 1) );
        //find min(nBytes, nPage, BUFFER_LENGTH) -- BUFFER_LENGTH is defined in the Wire library.
        nWrite = nBytes < nPage ? nBytes : nPage;
        nWrite = BUFFER_LENGTH - _nAddrBytes < nWrite ? BUFFER_LENGTH - _nAddrBytes : nWrite;
        ctrlByte = _eepromAddr | (byte) (addr >> _csShift);
        Wire.beginTransmission(ctrlByte);
        if (_nAddrBytes == 2) i2cWrite( (byte) (addr >> 8) );   //high addr byte
        i2cWrite( (byte) addr );                                //low addr byte
        i2cWrite(values, nWrite);
        txStatus = Wire.endTransmission();
        if (txStatus != 0) return txStatus;

        //wait up to 50ms for the write to complete
        for (uint8_t i=100; i; --i) {
            delayMicroseconds(500);                //no point in waiting too fast
            Wire.beginTransmission(ctrlByte);
            if (_nAddrBytes == 2) i2cWrite(0);      //high addr byte
            i2cWrite(0);                            //low addr byte
            txStatus = Wire.endTransmission();
            if (txStatus == 0) break;
        }
        if (txStatus != 0) return txStatus;

        addr += nWrite;         //increment the EEPROM address
        values += nWrite;       //increment the input data pointer
        nBytes -= nWrite;       //decrement the number of bytes left to write
    }
    return txStatus;
}

//Read bytes from external EEPROM.
//If the I/O would extend past the top of the EEPROM address space,
//a status of EEPROM_ADDR_ERR is returned. For I2C errors, the status
//from the Arduino Wire library is passed back through to the caller.
byte extEEPROM::read(unsigned long addr, byte *values, byte nBytes)
{
    byte ctrlByte;
    byte rxStatus;
    uint16_t nRead;             //number of bytes to read
    uint16_t nPage;             //number of bytes remaining on current page, starting at addr

    if (addr + nBytes > _totalCapacity) {   //will this read take us past the top of the EEPROM?
        return EEPROM_ADDR_ERR;             //yes, tell the caller
    }

    while (nBytes > 0) {
        nPage = _pageSize - ( addr & (_pageSize - 1) );
        nRead = nBytes < nPage ? nBytes : nPage;
        nRead = BUFFER_LENGTH < nRead ? BUFFER_LENGTH : nRead;
        ctrlByte = _eepromAddr | (byte) (addr >> _csShift);
        Wire.beginTransmission(ctrlByte);
        if (_nAddrBytes == 2) i2cWrite( (byte) (addr >> 8) );   //high addr byte
        i2cWrite( (byte) addr );                                //low addr byte
        rxStatus = Wire.endTransmission();
        if (rxStatus != 0) return rxStatus;        //read error

        Wire.requestFrom(ctrlByte, nRead);
        for (byte i=0; i<nRead; i++) values[i] = i2cRead();

        addr += nRead;          //increment the EEPROM address
        values += nRead;        //increment the input data pointer
        nBytes -= nRead;        //decrement the number of bytes left to write
    }
    return 0;
}
