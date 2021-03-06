# Timed Backgrounds

**Build Requirements:** autotools, glib, geoclue2, libxml

This is a set of timed backgrounds that change during the day.  The time
transitions are calculated by
[equations](http://www.srrb.noaa.gov/highlights/sunrise/calcdetails.html)
outlined by the U.S. Department of Commerce, National Oceanic and Atmospheric
Administration and implemented in
[Redshift](https://github.com/jonls/redshift).

Only tested on Cinnamon and GNOME desktop environments, but will work with any
other desktop environment that supports `gnome-backgrounds`.

Available backgrounds:
 * 24 hours (by [Arzamas](https://www.deviantart.com/arzamas/gallery))
 * Firewatch (by [Campo Santo](https://blog.camposanto.com/post/138965082204/firewatch-launch-wallpaper-when-we-redid-the) and [\_felics](https://www.reddit.com/r/Firewatch/comments/458ohf/i_made_a_night_version_of_the_launch_wallpaper/))
 * Island (by [arsenixc](https://arsenixc.deviantart.com/gallery/))
 * Metropolis (by [???](https://imgur.com/a/JH7RJ#2))
 * Mountainside (by [???](https://imgur.com/a/vqb7Q))


## Configuration

To compute you sunrise and sunset times, you can specify your latitude and
longitude manually, or you can use geoclue2 to compute it based on your IP
address. The geoclue2 option is much slower and requires a network connection.

If the config file does not exist, the program will default to using
`geoclue2`.


### Manual

Create a file called `~/.config/backgrounds.conf` and add:

```
[backgrounds]
location-provider=manual

[manual]
lat=<latitude>
lon=<longitude>
```

### Geoclue2 (default)

Create a file called `~/.config/backgrounds.conf` and add:

```
[backgrounds]
location-provider=geoclue
```

> Note: If using the geoclue2 option, make sure that your IP address is located
> where you are. If you are, for example, behind a VPN, the script will set the
> sunrise and sunset times corresponding to where the network servers are.

## Building

Run:
```
git clone https://github.com/sudorook/timed-backgrounds.git
cd timed-backgrounds
./autogen.sh
make
sudo make install
```

The backgrounds will be installed in `/usr/share/backgrounds/timed` and the
relevant metadata in `/usr/share/gnome-background-properties`,
`/usr/share/cinnamon-background-properties`, and
`/usr/share/mate-background-properties`.

Uninstall by running `sudo make uninstall`.

> Note: Times for sunrise and sunset vary throughout the year due to the tilt
> in Earth's axis. Recompile and reinstall the backgrounds periodically so that
> the transition times match real-world day/night cycles. The sunrise/sunset
> equations do not take into account elevation.

To rebuild and reinstall, run:
```
make clean
make
sudo make install
```


### GNOME

To select a wallpaper in GNOME, use "Backgrounds" in "System Settings".


### Cinnamon

Previously, setting timed backgrounds in Cinnamon was not possible using the
settings GUI, but this has now been
[fixed](https://github.com/linuxmint/Cinnamon/issues/5586). For Cinnamon
versions older than 4.4.3, use the following to set the background:

```
dconf write /org/cinnamon/desktop/background/picture-uri "'file:///usr/share/backgrounds/timed/<timed-background>.xml'"
```

> Note: By default, Cinnamon looks for the timed.xml file (which contains a
> list of all the available backgrounds) in
> `/usr/share/cinnamon-background-properties/`, but some distros (such as Arch)
> patch it to look in `/usr/share/gnome-background-properties/` instead.


### MATE (Untested)

Background metadata is installed in `/usr/share/mate-background-properties`. If
they cannot be selected via the mate background app, you will need to use dconf
to select one manually. See the above instructions for Cinnamon.
