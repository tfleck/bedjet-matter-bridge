// BedJet Matter Bridge — Web Flasher
const GITHUB_OWNER = "tfleck";
const GITHUB_REPO = "bedjet-matter-bridge";

// chip key -> the chipFamily name esp-web-tools expects.
const CHIP_FAMILIES = {
    esp32: "ESP32",
    esp32s2: "ESP32-S2",
    esp32s3: "ESP32-S3",
    esp32c3: "ESP32-C3",
    esp32c6: "ESP32-C6",
    esp32h2: "ESP32-H2",
};

function checkBrowserSupport() {
    const hasWebSerial = "serial" in navigator;
    const warning = document.getElementById("browserWarning");
    if (!hasWebSerial) {
        warning.style.display = "block";
    }
}

async function fetchLatestRelease() {
    const apiUrl = `https://api.github.com/repos/${GITHUB_OWNER}/${GITHUB_REPO}/releases/latest`;
    try {
        const response = await fetch(apiUrl, {
            headers: { "Accept": "application/vnd.github.v3+json" }
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return await response.json();
    } catch (error) {
        console.error("Failed to fetch release:", error);
        return null;
    }
}

// Match assets by exact name so "esp32" cannot match "esp32c6".
function buildsFromRelease(release) {
    const builds = [];
    for (const [chipKey, chipFamily] of Object.entries(CHIP_FAMILIES)) {
        const name = `firmware_${chipKey}_combined.bin`;
        const asset = release.assets.find(a => a.name.toLowerCase() === name);
        if (asset) {
            builds.push({
                chipFamily: chipFamily,
                parts: [{ path: asset.browser_download_url, offset: 0 }]
            });
        }
    }
    return builds;
}

// Fallback for when the GitHub API is unavailable/rate-limited: the Pages
// deploy stages every built image at firmware/<name> on the same origin, so
// probe those instead. Same-origin also avoids any release-asset CORS issues.
async function buildsFromStagedFirmware() {
    const builds = [];
    for (const [chipKey, chipFamily] of Object.entries(CHIP_FAMILIES)) {
        const url = new URL(`firmware/firmware_${chipKey}_combined.bin`, window.location.href).href;
        try {
            const res = await fetch(url, { method: "HEAD" });
            if (res.ok) {
                builds.push({
                    chipFamily: chipFamily,
                    parts: [{ path: url, offset: 0 }]
                });
            }
        } catch (_) {
            // Not staged - skip.
        }
    }
    return builds;
}

async function initInstallButton() {
    const versionBadge = document.getElementById("versionBadge");
    const installButton = document.getElementById("installButton");
    const chipSelect = document.getElementById("chipSelect");

    let version = "latest";
    let builds = [];

    const release = await fetchLatestRelease();
    if (release) {
        version = release.tag_name || version;
        builds = buildsFromRelease(release);
    }
    if (builds.length === 0) {
        builds = await buildsFromStagedFirmware();
    }

    if (builds.length === 0) {
        versionBadge.textContent = "Unavailable";
        versionBadge.style.background = "#f87171";
        return;
    }

    versionBadge.textContent = version;

    // Offer only the chips that actually have firmware for this release.
    chipSelect.innerHTML = '<option value="">Auto-detect (recommended)</option>';
    for (const build of builds) {
        const opt = document.createElement("option");
        opt.value = build.chipFamily;
        opt.textContent = build.chipFamily;
        chipSelect.appendChild(opt);
    }

    let manifestUrl = null;
    function showBuilds(selectedChip) {
        const subset = selectedChip
            ? builds.filter(b => b.chipFamily === selectedChip)
            : builds;
        if (manifestUrl) {
            URL.revokeObjectURL(manifestUrl);
        }
        const manifest = {
            name: "BedJet Matter Bridge",
            version: version,
            builds: subset,
        };
        manifestUrl = URL.createObjectURL(
            new Blob([JSON.stringify(manifest)], { type: "application/json" }));
        installButton.setAttribute("manifest", manifestUrl);
    }

    showBuilds("");
    chipSelect.addEventListener("change", () => showBuilds(chipSelect.value));
}

let serialPort = null;
let serialReader = null;
let serialKeepReading = false;
let consoleBuffer = "";

const connectBtn = document.getElementById("connectSerialBtn");
const disconnectBtn = document.getElementById("disconnectSerialBtn");
const serialOutput = document.getElementById("serialOutput");
const consoleOutput = document.getElementById("consoleOutput");

connectBtn.addEventListener("click", async () => {
    try {
        serialPort = await navigator.serial.requestPort();
        await serialPort.open({ baudRate: 115200 });
        serialKeepReading = true;
        serialOutput.style.display = "block";
        connectBtn.style.display = "none";
        readSerial();
    } catch (error) {
        console.error("Serial connection failed:", error);
        if (error.name !== "NotFoundError") {
            alert("Failed to connect: " + error.message);
        }
    }
});

disconnectBtn.addEventListener("click", async () => {
    serialKeepReading = false;
    if (serialReader) { await serialReader.cancel(); }
    if (serialPort) { await serialPort.close(); }
    serialOutput.style.display = "none";
    connectBtn.style.display = "inline-flex";
});

async function readSerial() {
    while (serialPort.readable && serialKeepReading) {
        serialReader = serialPort.readable.getReader();
        try {
            while (true) {
                const { value, done } = await serialReader.read();
                if (done) break;
                const text = new TextDecoder().decode(value);
                consoleBuffer += text;
                consoleOutput.textContent += text;
                document.getElementById("serialConsole").scrollTop = document.getElementById("serialConsole").scrollHeight;

                detectPairingInfo(consoleBuffer);

                if (consoleBuffer.length > 50000) {
                    consoleBuffer = consoleBuffer.slice(-25000);
                }
            }
        } catch (error) {
            console.error("Serial read error:", error);
        } finally {
            serialReader.releaseLock();
        }
    }
    serialOutput.style.display = "none";
    connectBtn.style.display = "inline-flex";
}

// The firmware prints "Manual pairing code: <11 digits>" and "QR payload: MT:..."
// as text (it never renders an ASCII QR), so only those are parsed.
function detectPairingInfo(text) {
    const manualMatch = text.match(/Manual pairing code:\s*(\d{11})/i);
    const qrMatch = text.match(/QR payload:\s*(\S+)/i);

    if (manualMatch) {
        document.getElementById("manualCode").textContent = manualMatch[1];
    }
    if (qrMatch) {
        document.getElementById("qrPayload").textContent = qrMatch[1];
        document.getElementById("qrPayloadRow").style.display = "block";
    }
    if (manualMatch || qrMatch) {
        document.getElementById("qrDisplay").style.display = "block";
    }
}

window.addEventListener("DOMContentLoaded", () => {
    checkBrowserSupport();
    initInstallButton();

    window.addEventListener("beforeunload", async () => {
        if (serialPort) {
            serialKeepReading = false;
            if (serialReader) await serialReader.cancel();
            await serialPort.close();
        }
    });
});
