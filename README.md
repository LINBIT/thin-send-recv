[![Open Source](https://img.shields.io/badge/Open-Source-brightgreen)](https://opensource.org/) [![GPLv3 License](https://img.shields.io/badge/License-GPL%20v3-brightgreen.svg)](https://opensource.org/licenses/) [![Slack Channel](https://img.shields.io/badge/Slack-Channel-brightgreen)](https://join.slack.com/t/linbit-community/shared_invite/enQtOTg0MTEzOTA4ODY0LTFkZGY3ZjgzYjEzZmM2OGVmODJlMWI2MjlhMTg3M2UyOGFiOWMxMmI1MWM4Yjc0YzQzYWU0MjAzNGRmM2M5Y2Q)  [![Active](http://img.shields.io/badge/Status-Active-brightgreen.svg)](https://linbit.com/drbd)

 # What is "thin_send & thin_recv"

This is a utility that mimics zfs send and zfs recv for the LVM thin world.

Its developed by [Philipp Reisner](https://www.linkedin.com/in/philipp-reisner-538569104/).

License is GPLv3, feel free to use & test.

## How it works

send / receive process can be triggered with the following command;

`$ thin_send ssd_vg/CentOS7.6 ssd_vg/li0 | ssh root@target-machine thin_recv kubuntu-vg/li0`

or you can use socat for streaming;

`target-machine$ socat TCP-LISTEN:4321 STDOUT | zstd -d | thin_recv kubuntu-vg/li0`

`source-machine$ thin_send ssd_vg/CentOS7.6 ssd_vg/li0 | zstd | socat STDIN TCP:10.43.8.39:4321`


## Support

thin_send & thin_recv is an open source software. You can use the slack channel below link to get support for individual use and development use.

[![Slack Channel](https://img.shields.io/badge/Slack-Channel-brightgreen)](https://join.slack.com/t/linbit-community/shared_invite/enQtOTg0MTEzOTA4ODY0LTFkZGY3ZjgzYjEzZmM2OGVmODJlMWI2MjlhMTg3M2UyOGFiOWMxMmI1MWM4Yjc0YzQzYWU0MjAzNGRmM2M5Y2Q) 

**Free Software, Hell Yeah!**

[![DRBD Powered by LINBIT](https://github.com/yusufyildiz/lstest2/blob/master/img/poweredby_linbit_small.png?raw=true)](https://www.linbit.com/linstor/)
