> [!IMPORTANT]
> **This fork is archived.** The `goodixtls55x4` driver for the Goodix
> **27c6:55b4** sensor (branch `55b4-experimental`) is replaced by
> **[libfprint-goodix55b4-milan](https://github.com/TheWeirdDev/libfprint-goodix55b4-milan)**.
> It uses Goodix's Milan matcher ported to native C instead of SIGFM, and is
> far more reliable: 95–97% first-attempt matches with no false accepts in
> testing.
>
> **Arch Linux:** install
> [`libfprint-goodix55b4`](https://aur.archlinux.org/packages/libfprint-goodix55b4)
> from the AUR. The old `libfprint-goodixtls-55x4` AUR package now installs it
> for you.
>
> ```
> yay -S libfprint-goodix55b4
> sudo systemctl restart fprintd
> fprintd-enroll
> ```
>
> **Other distributions:** there is no package yet. See the
> [new repository](https://github.com/TheWeirdDev/libfprint-goodix55b4-milan):
> `scripts/integrate.py` adds the driver to a libfprint source tree.
>
> **Moving over from this driver:**
> - Prints enrolled with this driver don't work with the new one. Re-enroll
>   with `fprintd-enroll`.
> - A sensor already flashed with
>   [goodix-fp-dump](https://github.com/mpi3d/goodix-fp-dump) needs nothing
>   else: it has the firmware (`GF3268_RTSEC_APP_10041`) and the zero key the
>   new driver expects. Otherwise, follow the
>   [provisioning steps](https://github.com/TheWeirdDev/libfprint-goodix55b4-milan#sensor-provisioning-one-time).
> - Other `GF3268_RTSEC_APP_` firmware (e.g. `10056`) passes the new driver's
>   firmware check but is untested.
> - The 55a4 was never supported by this driver and isn't by the new one.

<div align="center">

# LibFPrint

*LibFPrint is part of the **[FPrint][Website]** project.*

<br/>

[![Button Website]][Website]
[![Button Documentation]][Documentation]

[![Button Supported]][Supported]
[![Button Unsupported]][Unsupported]

[![Button Contribute]][Contribute]
[![Button Contributors]][Contributors]

</div>

## History

**LibFPrint** was originally developed as part of an
academic project at the **[University Of Manchester]**.

It aimed to hide the differences between consumer
fingerprint scanners and provide a single uniform
API to application developers.

## Goal

The ultimate goal of the **FPrint** project is to make
fingerprint scanners widely and easily usable under
common Linux environments.

## License

`Section 6` of the license states that for compiled works that use
this library, such works must include **LibFPrint** copyright notices
alongside the copyright notices for the other parts of the work.

**LibFPrint** includes code from **NIST's** **[NBIS]** software distribution.

We include **Bozorth3** from the **[US Export Controlled]**
distribution, which we have determined to be fine
being shipped in an open source project.

<br/>

<div align="right">

[![Badge License]][License]

</div>


<!----------------------------------------------------------------------------->

[Documentation]: https://fprint.freedesktop.org/libfprint-dev/
[Contributors]: https://gitlab.freedesktop.org/libfprint/libfprint/-/graphs/master
[Unsupported]: https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices
[Supported]: https://fprint.freedesktop.org/supported-devices.html
[Website]: https://fprint.freedesktop.org/

[Contribute]: ./HACKING.md
[License]: ./COPYING

[University Of Manchester]: https://www.manchester.ac.uk/
[US Export Controlled]: https://fprint.freedesktop.org/us-export-control.html
[NBIS]: http://fingerprint.nist.gov/NBIS/index.html


<!---------------------------------[ Badges ]---------------------------------->

[Badge License]: https://img.shields.io/badge/License-LGPL2.1-015d93.svg?style=for-the-badge&labelColor=blue


<!---------------------------------[ Buttons ]--------------------------------->

[Button Documentation]: https://img.shields.io/badge/Documentation-04ACE6?style=for-the-badge&logoColor=white&logo=BookStack
[Button Contributors]: https://img.shields.io/badge/Contributors-FF4F8B?style=for-the-badge&logoColor=white&logo=ActiGraph
[Button Unsupported]: https://img.shields.io/badge/Unsupported_Devices-EF2D5E?style=for-the-badge&logoColor=white&logo=AdBlock
[Button Contribute]: https://img.shields.io/badge/Contribute-66459B?style=for-the-badge&logoColor=white&logo=Git
[Button Supported]: https://img.shields.io/badge/Supported_Devices-428813?style=for-the-badge&logoColor=white&logo=AdGuard
[Button Website]: https://img.shields.io/badge/Homepage-3B80AE?style=for-the-badge&logoColor=white&logo=freedesktopDotOrg
