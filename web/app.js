// BedJet Matter Bridge — Web Flasher
const GITHUB_OWNER = "tfleck";
const GITHUB_REPO = "bedjet-matter-bridge";

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

function buildManifest(release) {
    const chipMap = {
        "esp32": "ESP32",
        "esp32s2": "ESP32-S2",
        "esp32s3": "ESP32-S3",
        "esp32c3": "ESP32-C3",
        "esp32c6": "ESP32-C6",
        "esp32h2": "ESP32-H2",
    };

    const builds = [];
    for (const [chipKey, chipFamily] of Object.entries(chipMap)) {
        const asset = release.assets.find(a =>
            a.name.toLowerCase().includes(chipKey) &&
            a.name.endsWith(".bin")
        );
        if (asset) {
            builds.push({
                chipFamily: chipFamily,
                parts: [{ path: asset.browser_download_url, offset: 0 }]
            });
        }
    }

    return {
        name: "BedJet Matter Bridge",
        version: release.tag_name,
        builds: builds,
    };
}

async function initInstallButton() {
    const versionBadge = document.getElementById("versionBadge");
    const installButton = document.getElementById("installButton");
    const chipSelect = document.getElementById("chipSelect");

    const release = await fetchLatestRelease();
    if (!release) {
        versionBadge.textContent = "Unavailable";
        versionBadge.style.background = "#f87171";
        return;
    }

    versionBadge.textContent = release.tag_name;

    const manifest = buildManifest(release);
    const manifestBlob = new Blob([JSON.stringify(manifest)], { type: "application/json" });
    const manifestUrl = URL.createObjectURL(manifestBlob);
    installButton.setAttribute("manifest", manifestUrl);

    chipSelect.addEventListener("change", (e) => {
        const selectedChip = e.target.value;
        if (selectedChip === "") {
            installButton.setAttribute("manifest", manifestUrl);
        } else {
            const filtered = {
                name: manifest.name,
                version: manifest.version,
                builds: manifest.builds.filter(b =>
                    b.chipFamily.toLowerCase().replace("-", "") ===
                    selectedChip.replace("-", "")
                )
            };
            const filteredBlob = new Blob([JSON.stringify(filtered)], { type: "application/json" });
            installButton.setAttribute("manifest", URL.createObjectURL(filteredBlob));
        }
    });
}

let serialPort = null;
let serialReader = null;
let serialKeepReading = false;
let consoleBuffer = "";
let qrCodeDetected = false;

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

                if (!qrCodeDetected) {
                    detectQRCode(consoleBuffer);
                }

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

function detectQRCode(text) {
    const manualMatch = text.match(/Manual pairing code:\s*(\d{11})/i);
    if (manualMatch) {
        document.getElementById("manualCode").textContent = manualMatch[1];
        document.getElementById("qrDisplay").style.display = "block";
    }

    const qrBlockMatch = text.match(/((█|#|\s){20,}\n){10,}/);
    if (qrBlockMatch && !qrCodeDetected) {
        document.getElementById("qrAscii").innerHTML = "<pre>" + qrBlockMatch[0] + "</pre>";
        document.getElementById("qrDisplay").style.display = "block";
        qrCodeDetected = true;
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
