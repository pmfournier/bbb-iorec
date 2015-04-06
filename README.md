Beaglebone iorec
================
Logic analyzer suite for Beaglebone Black

Post: http://hacks.pmf.io/2015/04/03/the-beaglebone-black-as-a-logic-analyzer/

### Connection diagram for infrared module


         |                     |
      ___|                 Vcc2|
     /   |                     |
     |   |                  TX<|
     \___|     Infrared        |
         |      module      RX>|############# P9_15 on Beaglebone
         |                     |
      ___|    TFDU 4101    Vcc1|############# P9_03 or P9_04 on Beaglebone
     /   |                     |    
     |   |                   SD|####
     \___|                     |   #
         |                  GND|############# P9_01 or P9_02 on Beaglebone
         |                     |
