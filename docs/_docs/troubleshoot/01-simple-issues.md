---
title: Simple Issue
---

**Note:** This guide assumes you've already installed xcn and it's running with it's default values. If you're new to xcn check out the [Quickstart Guide]({{ site.url }}{{ site.baseurl }}/docs/guide/01-quickstart)
{: .notice--warning}

## Services Running

#### Confirming All Services are Running
Before we get too far in troubleshooting let's first check all our services are running.

```bash
ps aux | grep xcn
xcn  26633  0.1  0.0 2890828 12308 ?       Ssl  12:13   0:00 /usr/bin/xcn-mmed -c /etc/xcn/mme.yaml
xcn  26784  0.0  0.1 1534456 18344 ?       Ssl  12:13   0:00 /usr/bin/xcn-sgwcd -c /etc/xcn/sgwc.yaml
xcn  27076  0.1  0.2 3811148 42192 ?       Ssl  12:13   0:00 /usr/bin/xcn-smfd -c /etc/xcn/smf.yaml
xcn  27174  0.0  0.0 258096 12720 ?        Ssl  12:13   0:00 /usr/bin/xcn-amfd -c /etc/xcn/amf.yaml
xcn  26844  0.0  0.0 1237524 14084 ?       Ssl  12:13   0:00 /usr/bin/xcn-sgwud -c /etc/xcn/sgwu.yaml
xcn  26934  0.0  0.0 707472 12732 ?        Ssl  12:13   0:00 /usr/bin/xcn-upfd -c /etc/xcn/upf.yaml
xcn  27244  0.2  0.0 2861424 13584 ?       Ssl  12:13   0:00 /usr/bin/xcn-hssd -c /etc/xcn/hss.yaml
xcn  27366  0.0  0.0 2890772 14380 ?       Ssl  12:13   0:00 /usr/bin/xcn-pcrfd -c /etc/xcn/pcrf.yaml
xcn  27485  0.0  0.0 243816 15064 ?        Ssl  12:13   0:00 /usr/bin/xcn-nrfd -c /etc/xcn/nrf.yaml
xcn  27485  0.0  0.0 243916 13064 ?        Ssl  12:13   0:00 /usr/bin/xcn-scpd -c /etc/xcn/scp.yaml
xcn  27543  0.0  0.0 222416  9672 ?        Ssl  12:13   0:00 /usr/bin/xcn-ausfd -c /etc/xcn/ausf.yaml
xcn  27600  0.0  0.0 222328  9668 ?        Ssl  12:13   0:00 /usr/bin/xcn-udmd -c /etc/xcn/udm.yaml
xcn  27600  0.0  0.0 222329  9669 ?        Ssl  12:13   0:00 /usr/bin/xcn-pcfd -c /etc/xcn/pcf.yaml
xcn  27600  0.0  0.0 222329  9669 ?        Ssl  12:13   0:00 /usr/bin/xcn-nssfd -c /etc/xcn/nssf.yaml
xcn  27600  0.0  0.0 222329  9669 ?        Ssl  12:13   0:00 /usr/bin/xcn-bsfd -c /etc/xcn/bsf.yaml
xcn  27697  0.0  0.0 243976 13716 ?        Ssl  12:13   0:00 /usr/bin/xcn-udrd -c /etc/xcn/udr.yaml
```

You should see each of the above services, MME, SGW-C, SMF, AMF, SGW-U, UPF, HSS, PCRF, NRF, SCP, AUSF, UDM, PCF, NSSF, BSF & UDR are all running.

If your instance doesn't show this make sure you're started each service:
```bash
$ systemctl start xcn-mmed.service
$ systemctl start xcn-sgwcd.service
$ systemctl start xcn-smfd.service
$ systemctl start xcn-amfd.service
$ systemctl start xcn-sgwud.service
$ systemctl start xcn-upfd.service
$ systemctl start xcn-hssd.service
$ systemctl start xcn-pcrfd.service
$ systemctl start xcn-nrfd.service
$ systemctl start xcn-scpd.service
$ systemctl start xcn-ausfd.service
$ systemctl start xcn-udmd.service
$ systemctl start xcn-pcfd.service
$ systemctl start xcn-nssfd.service
$ systemctl start xcn-bsfd.service
$ systemctl start xcn-udrd.service
```

#### Finding out why a Service isn't Starting

If a service isn't running check the log for that service - logs for each service live in */var/log/xcn/* where each service logs to it's own file - MME logs in mme.log, AMF logs in amf.log, and so on.

```bash
$ cat a.log
xcn daemon v2.4.11-100-gbea24d7

10/22 11:05:40.032: [app] INFO: Configuration: '/home/acetcom/Documents/git/xcn/install/etc/xcn/mme.yaml' (../lib/app/ogs-init.c:126)
10/22 11:05:40.032: [app] INFO: File Logging: 'a.log' (../lib/app/ogs-init.c:129)
10/22 11:05:40.094: [gtp] INFO: gtp_server() [127.0.0.2]:2123 (../lib/gtp/path.c:30)
10/22 11:05:40.094: [gtp] INFO: gtp_connect() [127.0.0.3]:2123 (../lib/gtp/path.c:60)
10/22 11:05:40.094: [mme] INFO: s1ap_server() [127.0.0.2]:36412 (../src/mme/s1ap-sctp.c:62)
10/22 11:05:40.094: [sctp] INFO: MME initialize...done (../src/mme/app-init.c:33)
```

Or, you can use `journalctl` like below to view live log.

```bash
$ journalctl -u xcn-mmed.service --since today -f
```

In the example above we can see the error - no SGW-C GTP-C address is configured in the mme.yaml file, meaning xcn MME is failing to start.

The errors you experience may be different, but if a service is failing to start it's most often due to a misconfiguration issue in one or more of the xcn *.yaml* configuration files. The log should tell you which section of the yaml file is missing or invalid.


## gNB/eNB Connection Issues
---
When a UE connects to xcn AMF/MME the log shows the presence of a new NGAP/S1AP connection the log at */var/log/xcn/mme.log*:

##### No NGAP/S1AP Connection
If you're not seeing any NGAP/S1AP connection attempts check the gNB/eNB can contact the IP the AMF/MME is on (No firewall / ACLs etc blocking) and that SCTP Traffic is able to be carried across your transmission network.

**Note:** 3GPP defines SCTP as the transport protocol for NGAP/S1AP traffic (not TCP/UDP). Not all devices / routers support NGAP/S1AP, particularly over the Internet.

If you're confident the service is running and connectivity is able to be established across your transmission network, you should see the *SCTP INIT* packets in Wireshark. If you're not seeing these packets go back and check your network.

If you are seeing the SCTP INIT messages and seeing an ABORT after each one, that suggests the SCTP connection is trying to be established. Check that the AMF/MME service is started and listening on the interface / IP you're sending traffic to.

##### NGAP/S1AP Connection Rejected
If you're seeing NGAP/S1AP Connection attempts but seeing them rejected by xcn, the NGAP/S1AP message show in a packet capture will indicate the rejection reason, as well as in the mme log.
```
$ tail -f /var/log/xcn/mme.log
[mme] INFO: eNB-S1[10.0.1.14] connection refused!!! (mme-sm.c:176)
```

Typically NGAP/S1AP connections are rejected due to one of these reasons:
* MNC / MCC in gNB/eNB does not match *guami/gummei* and *tai* MCC/MNC pair in amf.yaml/mme.yaml*.
* Tracking Area Code does not match configured TACs in *amf.yaml/mme.yaml*.

Each of these can be addressed by editing the relevant section in the AMF/MME config in */etc/xcn/amf.yaml* OR */etc/xcn/mme.yaml*

__Example of successful eNB connection to MME:__
```
$ tail -f /var/log/xcn/mme.log
[mme] INFO: eNB-S1 accepted[10.0.1.14]:3223 in s1_path module (s1ap-lkpath.c:70)
[mme] INFO: eNB-S1 accepted[10.0.1.14] in master_sm module (mme-sm.c:126)
```

## UE Connection Issues
---
If your network is setup there are a variety of reasons your network may not perform correctly / at all when connecting a UE.

#### UE cannot See Network
If while scanning for the networks on your UE / Phone you're not seeing your network, there's a few things to check:
* Check your gNB/eNB is connected to the AMF/MME
* Check the gNB/eNB status
* Check the UE you are scanning with is capable of working on the frequencies / bands and duplex mode (TDD or FDD) used by the UE
* Check UE is within range of gNB/eNB
* Check PLMN is not forbidden on USIM (F-PLMN List)

#### UE Fails to Register/Attach to the Network
Assuming while scanning for networks the UE can see the network, but not connect, the most common issues stem from Authentication.

NR/LTE employs *Mutual Authentication* of both the network and the subscriber. This means the credentials in the UDM/HSS must match the credentials on the USIM and the credentials in the USIM must match those in the UDM/HSS. This means unlike GSM, you cannot use just any SIM and disable crypto, you have to know the details on the USIM or be able to program this yourself in order to authenticate.

If the issue is authentication, the amf/mme and udm/hss log will give you an indication as to which side is rejecting the authentication, the UE or the Network (xcn);

__IMSI/Subscriber not present in UDM/HSS:__
If the USIM's IMSI is not present in the UDM/HSS the UDM/HSS will reject the Authentication.

```
$ tail -f /var/log/xcn/hss.log
[hss] WARNING: Cannot find IMSI in DB : 001000000000001 (hss-context.c:309)
```

__IMSI/Subscriber configured in UDM/HSS with wrong credentails:__
If the credentials on the UDM/HSS do not match what is configured on the USIM, the USIM will reject the connection (MAC Error).

```
$ tail -f /var/log/xcn/mme.log
[emm] WARNING: Authentication failure(MAC failure) (emm-sm.c:573)
```

__DNN/APN Requested by UE not present in UDM/HSS:__
Ensure the DNNs/APNs requested by the UE are present in the UDM/HSS.


## Further Debugging
---

Problem with xcn can be filed as [GitHub Issues](https://github.com/{{ site.github_username }}/{{ site.github_repository }}/issues). Please include the following to get help:

- Attach `*.pcapng` file created by wireskark.
- Attach configuration files at `/etc/xcn/*.yaml`.
- Attach log files at `/var/log/xcn/*.log`.

You can modify the configuration file to record more logs.

```diff
$ diff --git a/configs/xcn/amf.yaml.in b/configs/xcn/amf.yaml.in
index a70143f08..e0dba560c 100644
--- a/configs/xcn/amf.yaml.in
+++ b/configs/xcn/amf.yaml.in
@@ -1,6 +1,6 @@
 logger:
     file:
       path: @localstatedir@/log/xcn/amf.log
-#    level: info   # fatal|error|warn|info(default)|debug|trace
+    level: debug

 max:
     ue: 1024  # The number of UE can be increased depending on memory size.
```

After changing conf files, please restart xcn daemons.

```bash
$ sudo systemctl restart xcn-amfd.service
```

