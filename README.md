# qryptr
This repository contains all hardware and software to create a handheld qryptr device that secures text messages.
Applied cryptography can be complex and vulnerable. 
Side-channel attacks, spyware, and the complex ecosystem of smartphones hurts security of users.
We introduce a seperate, offline device that contains cryptographic keys, a keyboard, a camera and a screen. 
Encrypted messages are shared as QR codes using regular smartphones.
In case the smartphone is (partly) compromised, the security of the shared messages is not affected.

# architecture
![usage flow](./flow-diagram2.png)

Each user has a single handheld qryptr device.

Upon receiving the device, the user can generate his/her ECC keypair, her CryptoID.

The public key of the Crypto ID can be displayed onscreen as a QR code. Another user can add that CryptoID by scanning it. This is preferably done in-person.

After adding keys, users can write messages which are encrypted (AEAD), encoded and displayed as QR codes.

Using their smartphones, the users can photograph/share these QR-encoded, encrypted messages over their preferred messaging app, such as Signal or Whatsapp.

The receiving user can scan the shared photograph with his/her qryptr device. After selecting the recipient, the device will read, decode, decrypt and display the text message.

# use cases
-Sharing passwords between system administrators

-Sharing passwords for HSM procedures

-Sharing passwords for crypto wallets

-Sharing sensitive information between people

# implementation
## hardware
We chose the smallest possible platform to minimize platform complexity: the RP2040 microcontroller.

QR codes are read using a hardware camera, the GM-803. 

We are using the Sharp LS027B7DH01 display. 

Hardware designs are hosted on easyeda.com

## software
We use the Arduino IDE.

The cryptographic functions are implemented with the arduino Crypto library: https://www.arduino.cc/reference/en/libraries/crypto/

The interface uses https://github.com/olikraus/u8g2

# todos
## hardware
-Add resistor divider to measure the battery voltage
## software
-Create a software procedure to initialize the camera

# FAQ
-

