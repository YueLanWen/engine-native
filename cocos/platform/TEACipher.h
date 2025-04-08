#ifndef TEACIPHER_H
#define TEACIPHER_H

#include "../base/ccMacros.h"
#include "../base/CCData.h"
#include <stdint.h> // for sssize_t on android
#include <string>   // for sssize_t on linux
#include "CCStdC.h" // for sssize_t on window

class TEACipher
{
public:
    static TEACipher &getInstance();

    bool isEncryptedFile(const uint8_t *fileData, ssize_t dataSize);
    void encryptFile(const uint8_t *inputData, ssize_t inputSize, uint8_t *outputData, ssize_t &outputSize);
    void decryptFile(const uint8_t *inputData, ssize_t inputSize, cocos2d::Data &data);

private:
    TEACipher();
    TEACipher(const TEACipher &) = delete;
    TEACipher &operator=(const TEACipher &) = delete;

    void getFixedKey(uint8_t *key);
    uint32_t toUint32(const uint8_t *bytes) const;
    void fromUint32(uint32_t value, uint8_t *bytes) const;
    void teaEncrypt(const uint8_t *in, uint8_t *out, const uint8_t *key, int rounds);
    void teaDecrypt(const uint8_t *in, uint8_t *out, const uint8_t *key, int rounds = 32);
    void encryptIdentifier(const char *identifier, uint8_t *encryptedIdentifier);
    void decryptIdentifier(const uint8_t *encryptedIdentifier, char *decryptedIdentifier);

    static const uint8_t keyPart1[8];
    static const uint8_t keyPart2[8];
};

#endif // TEACIPHER_H