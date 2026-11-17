# B2W

Convert **World at War** custom Zombies maps into **Black Ops II** Zombies mods for Plutonium T6.

**[Download for Windows](https://github.com/Ticass/B2W/releases/latest/download/WawConverter-Windows.zip)** · **[Download for Linux](https://github.com/Ticass/B2W/releases/latest/download/WawConverter-Linux-x86_64.tar.gz)** · [Map compatibility](https://ticass.github.io/B2W/)

![B2W desktop builder with Cheese Cube Unlimited selected](docs/images/b2w-map-builder.png)

## Build your first map

You’ll need World at War, Black Ops II, BO2 Mod Tools, and Plutonium T6.

1. Extract the download and open `WawConverter.exe` on Windows. On Linux, follow the [Linux setup guide](docs/LINUX.md).
2. Check your game paths in **Setup**, then select the map’s main `.ff` from its original WaW mod folder.
3. Click **Build Map**. When it finishes, choose **Install to Plutonium**, then **Launch Map**.

The first build prepares shared game assets. Compatibility varies by map. The **Reports** tab shows what converted and what needs attention.

## From WaW to BO2

```mermaid
flowchart LR
    A["WaW map"] --> B["Extract assets"]
    B --> C["Translate and compile"]
    C --> D["BO2 Zombies mod"]
```

## Make it yours

Set the map’s title, location, and artwork, then preview how they’ll look in BO2’s menus.

![B2W artwork editor previewing the map selection globe](docs/images/b2w-artwork-preview.png)

[Desktop guide](docs/DESKTOP.md) · [Setup and troubleshooting](docs/USAGE.md) · [Release notes](CHANGELOG.md) · [Development](docs/DEVELOPMENT.md)
