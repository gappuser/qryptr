# qryptr
This repository contains all hardware and software to create a handheld device that secures text messages.
Applied cryptography can be complex and vulnerable. Side-channel attacks, spyware, and the complex ecosystem of smartphones hurts security of users.
We introduce a seperate, offline device that contains cryptographic keys, a keyboard, a camera and a screen. Encrypted messages are shared as QR codes using regular, possibly compromised smartphones.

# architecture
Be

# use cases
-Sharing passwords between system administrators
-Sharing passwords for HSM procedures
-Sharing passwords for crypto wallets
-Sharing sensitive information between journalists and politicians

# implementation
## hardware
We chose the smallest possible platform to minimize platform complexity: the RP2040 microcontroller. QR codes are read using a hardware camera, the GM-803. We are using the Sharp LS027B7DH01 display. 
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

