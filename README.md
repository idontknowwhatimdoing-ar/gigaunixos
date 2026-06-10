# What I Think Is The First Unix-Like OS On The Arduino Giga

I started this out one day because yknow, bored. I have ADHD so I eventually forgot all about it. I am a little ashamed about it since I used AI and I know how people feel about it, I just hate coding, my brain just doesnt work that way; I find coding too menial for me. I am releasing it here because I think its far too interesting to just let rot in forgotten memory hell. I just want somebody to make a fork of it and water down the cardboard castle that was and rebuild it properly.

I tried to base it off of unix and threading with a philosophy around modularity and threading, and also still being able to do some scheduling stuff. Obviously to run this, you need the Arduino Giga, the display shield, and preferably a keyboard.

Heres some stuff it has:
- You can type in commands from arduino IDE's serial monitor if you wish instead of using a keyboard
- GBasic, a coding language that was based off of.. BASIC!!!!!!!!! because I partially understand how to code in basic on my own without too much help
- Chron but evil
- An actual help command and paging the listed commands
- Threading
- RTC functionality plus NTP
- Actual pinging
- Memory allocation
- CPU stats
- Board stats
- Terminal based, and such.
- One of the few projects that I know of thats released online that actually uses the giga display shield
- Basic SDRAM based file system (I was planning to make it so it can talk to an SD card reader however I dont have one...)
- Direct GPIO controls in the terminal (read write etc)
- Fuzzy command matching

Unfortunately theres still some of issues:
- I dont think GBasic scripts can run for some reason but I've not tested it thoroughly. I think theres still a lot of issues with it though.
- Keyboard likes to drop keys (I'm not certain but I believe its because of how the keyboard is interacted with)
- I dont believe wifi works
- More probably

Ultimately I understand and enjoy OS's and hardware. I just cant stand coding whatsoever.
