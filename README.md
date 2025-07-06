Rough prototype of IoT device using an ESP32 development board. This is no longer working as various API keys have expired.

It has been over 2 years since this project was touched so I will show various videos below of it being demo: 

Website Demo Link: https://youtu.be/1oexJyhdpjg
The link above shows a quick demonstration of the website created using NextJS. The login page was created using the next-auth library to allow sign ins via Google or a custom user/password or through email. 

Hardware Demo (Polling Database for updates): https://youtu.be/xAJtARbXMEY
The link above shows a demonstration of the app (created with React Native) and the website (created with NextJS) and demonstrates the polling method used to check if the database has been updated and if so, updates the UI accordingly. The LEDs are updated to show that communication between the hardware (ESP32) and the database (MongoDB) is working.




With this project, I learned how to implement HTTPS, MQTT, BLE, work with a database, and WiFi provisioning. Since this project was worked on intermittently and documented late, I don't recall the current state of the code. At some point, I implemented protobufs (reading through Espressif's documents) using BLE to have a custom wifi provision protocol to send over SSID and Password for when trying to connect to an access point. I recall this being buggy as it would sometimes not take the data due to interference, possibly. It would work at my workplace but have issues on my home network.

The method of data handling was inefficient reflecting back on what I had originally. So users would have their names and emails stored into a database (MongoDB) and this data was taken from Google's email login service which would provide public info that can be fetched after a user signs in with Google. With the user data, the database can now know what devices a user has saved onto their account. The devices will be displayed to the user on a device page on a website. For a user to register a device, they need to see the serial number which would show up on a display/or read through the printf statements during prototyping. To verify if the device exists, the device will power on and read the database and see if its serial number has been registered into the database. If it doesn't see its serial number then it makes a POST request via HTTPS and registers itself into the database. After this the user can assign the device to their account so that they have access to remotely control the device via the website or mobile app. For security, a random code is also required for a user to register a device. They will provide 3 parameters: Name of the Device, Security code, Serial Number. After this the server will check if the securitiy code and serial number are valid and assign the device to the user. With this the user can press buttons on the UI to configure the device into two different modes and remotely toggle a relay. 
