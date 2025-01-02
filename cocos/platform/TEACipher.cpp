#include "TEACipher.h"
#include "../base/ccMacros.h"
#include "../base/CCData.h"
#include <stdint.h> // for sssize_t on android
#include <string>   // for sssize_t on linux
#include "CCStdC.h" // for sssize_t on window
// 固定密钥片段
const uint8_t TEACipher::keyPart1[8] = {0x4b, 0x3c, 0x6f, 0x70, 0x4d, 0x2e, 0x1a, 0x7d};
const uint8_t TEACipher::keyPart2[8] = {0x8f, 0x9e, 0x3c, 0x1b, 0x2a, 0x4d, 0x6f, 0x8e};

// 获取单例实例
TEACipher &TEACipher::getInstance()
{
    static TEACipher instance;
    return instance;
}

// 构造函数
TEACipher::TEACipher() {}

// 合并密钥片段
void TEACipher::getFixedKey(uint8_t *key)
{
    memcpy(key, keyPart1, 8);
    memcpy(key + 8, keyPart2, 8);
}

// 将字节数组转换为 uint32_t
uint32_t TEACipher::toUint32(const uint8_t *bytes) const
{
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

// 将 uint32_t 转换为字节数组
void TEACipher::fromUint32(uint32_t value, uint8_t *bytes) const
{
    bytes[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    bytes[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[3] = static_cast<uint8_t>(value & 0xFF);
}

// TEA加密算法实现
void TEACipher::teaEncrypt(const uint8_t *in, uint8_t *out, const uint8_t *key, int rounds)
{
    uint32_t v0 = toUint32(in);
    uint32_t v1 = toUint32(in + 4);
    uint32_t k0 = toUint32(key);
    uint32_t k1 = toUint32(key + 4);
    uint32_t k2 = toUint32(key + 8);
    uint32_t k3 = toUint32(key + 12);
    uint32_t delta = 0x9E3779B9;
    uint32_t sum = 0;

    for (int i = 0; i < rounds; i++)
    {
        sum += delta;
        v0 += ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        v1 += ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
    }

    fromUint32(v0, out);
    fromUint32(v1, out + 4);
}

// TEA解密算法实现
void TEACipher::teaDecrypt(const uint8_t *in, uint8_t *out, const uint8_t *key, int rounds)
{
    uint32_t v0 = toUint32(in);
    uint32_t v1 = toUint32(in + 4);
    uint32_t k0 = toUint32(key);
    uint32_t k1 = toUint32(key + 4);
    uint32_t k2 = toUint32(key + 8);
    uint32_t k3 = toUint32(key + 12);
    uint32_t delta = 0x9E3779B9;
    uint32_t sum = (rounds * delta);

    for (int i = 0; i < rounds; i++)
    {
        v1 -= ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
        v0 -= ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        sum -= delta;
    }

    fromUint32(v0, out);
    fromUint32(v1, out + 4);
}

// 简单加密标识符
void TEACipher::encryptIdentifier(const char *identifier, uint8_t *encryptedIdentifier)
{
    const uint8_t key = 0xAB; // 简单的异或密钥
    for (int i = 0; i < 4; ++i)
    {
        encryptedIdentifier[i] = identifier[i] ^ key;
    }
}

// 简单解密标识符
void TEACipher::decryptIdentifier(const uint8_t *encryptedIdentifier, char *decryptedIdentifier)
{
    const uint8_t key = 0xAA; // 简单的异或密钥
    for (int i = 0; i < 4; ++i)
    {
        decryptedIdentifier[i] = encryptedIdentifier[i] ^ key;
    }
    decryptedIdentifier[4] = '\0'; // 确保字符串终止
}

// 检查文件是否加密
bool TEACipher::isEncryptedFile(const uint8_t *fileData, ssize_t dataSize)
{
    if (dataSize < 4)
        return false;

    char decryptedIdentifier[5];
    decryptIdentifier(fileData, decryptedIdentifier);
    return strncmp(decryptedIdentifier, "GaMi", 4) == 0;
}

// 加密文件
void TEACipher::encryptFile(const uint8_t *inputData, ssize_t inputSize, uint8_t *outputData, ssize_t &outputSize)
{
    uint8_t key[16];
    getFixedKey(key);

    const char *identifier = "GaMi";
    uint8_t encryptedIdentifier[4];
    encryptIdentifier(identifier, encryptedIdentifier);

    ssize_t paddedLength = (inputSize + 7) / 8 * 8; // 8字节对齐
    outputSize = 4 + paddedLength;

    memcpy(outputData, encryptedIdentifier, 4);
    memcpy(outputData + 4, inputData, inputSize);

    for (size_t i = 0; i < paddedLength; i += 8)
    {
        teaEncrypt(outputData + 4 + i, outputData + 4 + i, key, 32);
    }
}

// 解密文件
void TEACipher::decryptFile(const uint8_t *inputData, ssize_t inputSize, cocos2d::Data &outputData)
{
    uint8_t key[16];
    getFixedKey(key);

    if (inputSize < 4)
        return;

    ssize_t dataSize = inputSize - 4; // 忽略前4个字节的标识符
    const uint8_t *encryptedData = inputData + 4;
    uint8_t *outData = (unsigned char *)malloc(sizeof(unsigned char) * dataSize);
    for (ssize_t i = 0; i < dataSize; i += 8)
    {
        teaDecrypt(encryptedData + i, outData + i, key);
    }
    outputData.copy(outData, dataSize);
    free(outData);
}
