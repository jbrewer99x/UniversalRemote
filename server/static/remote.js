let currentDevice = null;
let selectionGeneration = 0;
let statusRefreshInFlight = false;

const deviceSelect = document.getElementById("deviceSelect");
const inputCard = document.getElementById("input-card");
const inputTabs = document.getElementById("inputTabs");
const onlineStatus = document.getElementById("onlineStatus");
const powerStatus = document.getElementById("powerStatus");
const inputStatus = document.getElementById("inputStatus");
const volumeStatus = document.getElementById("volumeStatus");
const connectButton = document.getElementById("connectButton");
const findRemoteButton = document.getElementById("findRemoteButton");
const message = document.getElementById("message");
const headerStatus = document.getElementById("status");
const parentalDeviceSelect = document.getElementById("parentalDeviceSelect");
const parentalEnabled = document.getElementById("parentalEnabled");
const parentalAccessStatus = document.getElementById("parentalAccessStatus");
const parentalTimezone = document.getElementById("parentalTimezone");
const parentalSchedule = document.getElementById("parentalSchedule");
const saveParentalControlsButton = document.getElementById("saveParentalControls");

const PARENTAL_DAYS = [
    ["monday", "Monday"], ["tuesday", "Tuesday"], ["wednesday", "Wednesday"],
    ["thursday", "Thursday"], ["friday", "Friday"], ["saturday", "Saturday"],
    ["sunday", "Sunday"]
];
let parentalControlsData = null;

function clearStatusClasses(element) {
    if (!element) return;
    element.classList.remove("status-green", "status-yellow", "status-red");
}

function setMessage(value) {
    if (message) message.textContent = value;
}

function hideInputs() {
    inputTabs.innerHTML = "";
    inputCard.hidden = true;
}

function showInputs() {
    inputCard.hidden = false;
}

async function api(url, options = {}) {
    const response = await fetch(url, {
        cache: "no-store",
        headers: {"Content-Type": "application/json"},
        ...options
    });

    if (!response.ok) {
        const body = await response.json().catch(() => ({}));
        throw new Error(body.detail || `HTTP ${response.status}`);
    }

    return response.json();
}

function renderStatus(status) {
    clearStatusClasses(onlineStatus);

    if (status.paired === false) {
        onlineStatus.textContent = "Not paired";
        onlineStatus.classList.add("status-yellow");
    } else if (status.online) {
        onlineStatus.textContent = "Online";
        onlineStatus.classList.add("status-green");
    } else {
        onlineStatus.textContent = "Offline";
        onlineStatus.classList.add("status-red");
    }

    clearStatusClasses(powerStatus);

    if (status.power === true || status.power === "on") {
        powerStatus.textContent = "On";
        powerStatus.classList.add("status-green");
    } else if (status.power === false || status.power === "off") {
        powerStatus.textContent = "Off";
        powerStatus.classList.add("status-red");
    } else {
        powerStatus.textContent = "Unknown";
    }

    const input = status.input_label ||
        (
            status.input && typeof status.input === "object"
                ? status.input.id || status.input.name
                : status.input
        ) ||
        "--";

    inputStatus.textContent = currentDevice === "pc" ? "--" : input;

    const volume =
        status.volume && typeof status.volume === "object"
            ? status.volume.display ?? status.volume.raw
            : status.volume;

    volumeStatus.textContent =
        volume !== undefined && volume !== null
            ? `${volume}${status.muted || status.mute === true ? " (Muted)" : ""}`
            : "--";

    if (status.error) {
        setMessage(`Error: ${status.error}`);
    }
}

async function loadDevices() {
    const devices = await api("/api/devices");

    deviceSelect.innerHTML = "";

    devices.forEach(device => {
        const option = document.createElement("option");
        option.value = device.id;
        option.textContent = device.name;
        deviceSelect.appendChild(option);
    });

    if (devices.length) {
        await selectDevice(devices[0].id);
    }
}

async function selectDevice(deviceId) {
    currentDevice = deviceId;
    deviceSelect.value = deviceId;

    const myGeneration = ++selectionGeneration;

    hideInputs();

    inputStatus.textContent = "--";
    volumeStatus.textContent = "--";
    onlineStatus.textContent = "Checking...";
    powerStatus.textContent = "Unknown";

    if (deviceId === "pc") {
        await refreshSelectedStatus(myGeneration);
        return;
    }

    await Promise.allSettled([
        refreshSelectedStatus(myGeneration),
        loadSelectedInputs(myGeneration)
    ]);
}

async function refreshSelectedStatus(
    generation = selectionGeneration
) {
    if (!currentDevice || statusRefreshInFlight) return;

    const selected = currentDevice;
    statusRefreshInFlight = true;

    try {
        const status = await api(
            `/api/devices/${encodeURIComponent(selected)}/status`
        );

        if (
            generation !== selectionGeneration ||
            selected !== currentDevice
        ) {
            return;
        }

        renderStatus(status);

        headerStatus.textContent =
            `Updated ${new Date().toLocaleTimeString()}`;

    } catch (error) {
        if (
            generation !== selectionGeneration ||
            selected !== currentDevice
        ) {
            return;
        }

        clearStatusClasses(onlineStatus);

        onlineStatus.textContent = "Offline";
        onlineStatus.classList.add("status-red");

        headerStatus.textContent =
            `Connection error: ${error.message}`;

    } finally {
        statusRefreshInFlight = false;
    }
}

async function loadSelectedInputs(
    generation = selectionGeneration
) {
    if (!currentDevice || currentDevice === "pc") {
        hideInputs();
        inputStatus.textContent = "--";
        return;
    }

    const selected = currentDevice;

    try {
        const data = await api(
            `/api/devices/${encodeURIComponent(selected)}/inputs`
        );

        if (
            generation !== selectionGeneration ||
            selected !== currentDevice
        ) {
            return;
        }

        inputTabs.innerHTML = "";

        if (
            !Array.isArray(data.inputs) ||
            data.inputs.length === 0
        ) {
            hideInputs();
            return;
        }

        showInputs();

        data.inputs.forEach(input => {
            const button = document.createElement("button");
            button.className = "inputButton";

            const inputId =
                typeof input === "string"
                    ? input
                    : input.id;

            const inputLabel =
                typeof input === "string"
                    ? input
                    : input.label || input.name || input.id;

            button.textContent = inputLabel;

            const currentId =
                data.current &&
                typeof data.current === "object"
                    ? data.current.id
                    : data.current;

            if (inputId === currentId) {
                button.classList.add("active");
            }

            button.addEventListener("click", async () => {
                const targetDevice = currentDevice;
                const targetGeneration = selectionGeneration;

                try {
                    setMessage(`Switching to ${inputLabel}...`);

                    await api(
                        `/api/devices/${
                            encodeURIComponent(targetDevice)
                        }/inputs/${
                            encodeURIComponent(inputId)
                        }`,
                        {
                            method: "POST"
                        }
                    );

                    if (
                        targetDevice !== currentDevice ||
                        targetGeneration !== selectionGeneration
                    ) {
                        return;
                    }

                    setMessage(
                        `Input changed to ${inputLabel}`
                    );

                    await Promise.allSettled([
                        refreshSelectedStatus(
                            targetGeneration
                        ),
                        loadSelectedInputs(
                            targetGeneration
                        )
                    ]);

                } catch (error) {
                    if (targetDevice === currentDevice) {
                        setMessage(
                            `Error: ${error.message}`
                        );
                    }
                }
            });

            inputTabs.appendChild(button);
        });

    } catch (error) {
        if (
            generation !== selectionGeneration ||
            selected !== currentDevice
        ) {
            return;
        }

        hideInputs();
    }
}

function sendCommand(command) {
    if (!currentDevice) return;

    const selected = currentDevice;

    fetch("/api/command", {
        method: "POST",
        cache: "no-store",
        headers: {
            "Content-Type": "application/json"
        },
        body: JSON.stringify({
            device: selected,
            command
        })
    })
        .then(async response => {
            if (!response.ok) {
                const body =
                    await response.json().catch(
                        () => ({})
                    );

                throw new Error(
                    body.detail ||
                    `HTTP ${response.status}`
                );
            }

            if (selected === currentDevice) {
                setMessage(
                    command.replaceAll("_", " ")
                );
            }
        })
        .catch(error => {
            if (selected === currentDevice) {
                setMessage(
                    `Error: ${error.message}`
                );
            }
        });
}

async function findRemote() {
    if (!findRemoteButton) return;

    findRemoteButton.disabled = true;
    setMessage("Finding remote...");

    try {
        await api("/api/remote/find", {
            method: "POST"
        });

        setMessage("Find Remote activated");

    } catch (error) {
        setMessage(`Error: ${error.message}`);

    } finally {
        findRemoteButton.disabled = false;
    }
}

async function connectDevice() {
    if (!currentDevice) return;

    const selected = currentDevice;
    const generation = selectionGeneration;

    try {
        setMessage("Connecting...");

        await api(
            `/api/devices/${
                encodeURIComponent(selected)
            }/connect`,
            {
                method: "POST"
            }
        );

        if (
            selected !== currentDevice ||
            generation !== selectionGeneration
        ) {
            return;
        }

        await refreshSelectedStatus(generation);

        setMessage("Connected");

    } catch (error) {
        if (selected === currentDevice) {
            setMessage(
                `Error: ${error.message}`
            );
        }
    }
}


// ---------------------------------------------------------
// Parental Controls
// ---------------------------------------------------------

function createScheduleWindow(
    day,
    start = "",
    end = ""
) {
    const row = document.createElement("div");

    row.className =
        "metric parentalWindow";

    row.dataset.day = day;

    const startInput =
        document.createElement("input");

    startInput.type = "time";
    startInput.value = start;
    startInput.className = "parentalStart";

    startInput.setAttribute(
        "aria-label",
        `${day} start time`
    );

    const endInput =
        document.createElement("input");

    endInput.type = "time";
    endInput.value = end;
    endInput.className = "parentalEnd";

    endInput.setAttribute(
        "aria-label",
        `${day} end time`
    );

    const times =
        document.createElement("span");

    times.append(
        startInput,
        document.createTextNode(" → "),
        endInput
    );

    const removeButton =
        document.createElement("button");

    removeButton.type = "button";
    removeButton.className = "remoteButton";
    removeButton.textContent = "Remove";

    removeButton.addEventListener(
        "click",
        () => row.remove()
    );

    row.append(
        times,
        removeButton
    );

    return row;
}

function renderParentalSchedule(rule = {}) {
    if (!parentalSchedule) return;

    parentalSchedule.innerHTML = "";

    const schedule =
        rule.schedule || {};

    PARENTAL_DAYS.forEach(
        ([day, label]) => {

            const daySection =
                document.createElement("div");

            daySection.className =
                "parentalDay";

            const heading =
                document.createElement("h3");

            heading.textContent = label;

            daySection.appendChild(
                heading
            );

            const windows =
                document.createElement("div");

            windows.className =
                "parentalWindows";

            windows.dataset.day = day;

            (schedule[day] || []).forEach(
                window => {
                    windows.appendChild(
                        createScheduleWindow(
                            day,
                            window.start,
                            window.end
                        )
                    );
                }
            );

            daySection.appendChild(
                windows
            );

            const addButton =
                document.createElement("button");

            addButton.type = "button";

            addButton.className =
                "actionButton fullWidth";

            addButton.textContent =
                `Add ${label} Window`;

            addButton.addEventListener(
                "click",
                () => {
                    windows.appendChild(
                        createScheduleWindow(
                            day,
                            "16:00",
                            "21:00"
                        )
                    );
                }
            );

            daySection.appendChild(
                addButton
            );

            parentalSchedule.appendChild(
                daySection
            );
        }
    );
}

function renderSelectedParentalRule() {
    if (
        !parentalControlsData ||
        !parentalDeviceSelect
    ) {
        return;
    }

    const deviceId =
        parentalDeviceSelect.value;

    const rule =
        parentalControlsData.devices?.[
            deviceId
        ] || {
            enabled: false,
            schedule: {},
            allowed_now: true
        };

    parentalEnabled.checked =
        rule.enabled === true;

    parentalAccessStatus.textContent =
        rule.enabled
            ? (
                rule.allowed_now
                    ? "Allowed"
                    : "Blocked"
            )
            : "Schedule disabled";

    parentalAccessStatus.classList.toggle(
        "status-green",
        rule.enabled && rule.allowed_now
    );

    parentalAccessStatus.classList.toggle(
        "status-red",
        rule.enabled && !rule.allowed_now
    );

    parentalTimezone.textContent =
        parentalControlsData.timezone ||
        "--";

    renderParentalSchedule(rule);
}

async function loadParentalControls() {
    if (!parentalDeviceSelect) return;

    try {
        const data =
            await api(
                "/api/parental-controls"
            );

        const previous =
            parentalDeviceSelect.value;

        parentalControlsData = data;

        parentalDeviceSelect.innerHTML =
            "";

        Object.entries(
            data.devices || {}
        ).forEach(
            ([deviceId, info]) => {

                const option =
                    document.createElement(
                        "option"
                    );

                option.value =
                    deviceId;

                option.textContent =
                    info.name ||
                    deviceId;

                parentalDeviceSelect.appendChild(
                    option
                );
            }
        );

        if (
            previous &&
            data.devices?.[previous]
        ) {
            parentalDeviceSelect.value =
                previous;
        }

        renderSelectedParentalRule();

    } catch (error) {
        setMessage(
            `Parental controls error: ${
                error.message
            }`
        );
    }
}

function collectParentalSchedule() {
    const schedule = {};

    PARENTAL_DAYS.forEach(
        ([day]) => {

            const windows = [];

            parentalSchedule
                .querySelectorAll(
                    `.parentalWindows[data-day="${day}"] .parentalWindow`
                )
                .forEach(row => {

                    const start =
                        row.querySelector(
                            ".parentalStart"
                        )?.value;

                    const end =
                        row.querySelector(
                            ".parentalEnd"
                        )?.value;

                    if (start && end) {
                        windows.push({
                            start,
                            end
                        });
                    }
                });

            if (windows.length) {
                schedule[day] =
                    windows;
            }
        }
    );

    return schedule;
}

async function saveParentalControls() {
    if (
        !parentalDeviceSelect?.value ||
        !saveParentalControlsButton
    ) {
        return;
    }

    const deviceId =
        parentalDeviceSelect.value;

    saveParentalControlsButton.disabled =
        true;

    try {
        await api(
            `/api/parental-controls/${
                encodeURIComponent(deviceId)
            }`,
            {
                method: "PUT",
                body: JSON.stringify({
                    enabled:
                        parentalEnabled.checked,
                    schedule:
                        collectParentalSchedule()
                })
            }
        );

        const deviceName =
            parentalDeviceSelect.options[
                parentalDeviceSelect.selectedIndex
            ]?.text || deviceId;

        setMessage(
            `Parental schedule saved for ${
                deviceName
            }`
        );

        await loadParentalControls();

    } catch (error) {
        setMessage(
            `Parental controls error: ${
                error.message
            }`
        );

    } finally {
        saveParentalControlsButton.disabled =
            false;
    }
}


// ---------------------------------------------------------
// Living Room Lights
// ---------------------------------------------------------

const LIGHT_DEVICE =
    "living_room_lights";

const lightBrightness =
    document.getElementById(
        "lightBrightness"
    );

const lightBrightnessValue =
    document.getElementById(
        "lightBrightnessValue"
    );

const lightColor =
    document.getElementById(
        "lightColor"
    );

const setLightColorButton =
    document.getElementById(
        "setLightColor"
    );

const crazyModeButton =
    document.getElementById(
        "crazyModeButton"
    );

let crazyModeEnabled = false;

async function sendLightCommand(
    command,
    value = null
) {
    try {
        setMessage(
            `Lights: ${
                command.replaceAll("_", " ")
            }...`
        );

        await api(
            "/api/command",
            {
                method: "POST",
                body: JSON.stringify({
                    device:
                        LIGHT_DEVICE,
                    command,
                    value
                })
            }
        );

        setMessage(
            `Lights: ${
                command.replaceAll("_", " ")
            }`
        );

    } catch (error) {
        setMessage(
            `Light error: ${
                error.message
            }`
        );
    }
}

function hexToRgb(hex) {
    const value =
        hex.replace("#", "");

    return {
        r: parseInt(
            value.slice(0, 2),
            16
        ),
        g: parseInt(
            value.slice(2, 4),
            16
        ),
        b: parseInt(
            value.slice(4, 6),
            16
        )
    };
}

async function setCrazyMode(enabled) {
    if (!crazyModeButton) return;

    crazyModeButton.disabled = true;

    try {
        const result =
            await api(
                "/api/command",
                {
                    method: "POST",
                    body: JSON.stringify({
                        device:
                            LIGHT_DEVICE,
                        command:
                            enabled
                                ? "crazy_on"
                                : "crazy_off"
                    })
                }
            );

        crazyModeEnabled =
            result?.result?.crazy_mode ??
            enabled;

        crazyModeButton.textContent =
            `Crazy Mode: ${
                crazyModeEnabled
                    ? "ON"
                    : "OFF"
            }`;

        crazyModeButton.classList.toggle(
            "active",
            crazyModeEnabled
        );

        setMessage(
            `Crazy Mode ${
                crazyModeEnabled
                    ? "ON"
                    : "OFF"
            }`
        );

    } catch (error) {
        setMessage(
            `Light error: ${
                error.message
            }`
        );

    } finally {
        crazyModeButton.disabled = false;
    }
}

async function refreshCrazyModeStatus() {
    if (!crazyModeButton) return;

    try {
        const status =
            await api(
                `/api/devices/${
                    encodeURIComponent(
                        LIGHT_DEVICE
                    )
                }/status`
            );

        crazyModeEnabled =
            status.crazy_mode === true;

        crazyModeButton.textContent =
            `Crazy Mode: ${
                crazyModeEnabled
                    ? "ON"
                    : "OFF"
            }`;

        crazyModeButton.classList.toggle(
            "active",
            crazyModeEnabled
        );

    } catch (_) {
        // Leave the last known button state alone
        // if a status query is missed.
    }
}

if (crazyModeButton) {
    crazyModeButton.addEventListener(
        "click",
        () => setCrazyMode(
            !crazyModeEnabled
        )
    );

    refreshCrazyModeStatus();

    setInterval(
        refreshCrazyModeStatus,
        5000
    );
}

document
    .querySelectorAll(
        "[data-light-command]"
    )
    .forEach(button => {

        button.addEventListener(
            "click",
            () => {
                const command =
                    button.dataset
                        .lightCommand;

                let value =
                    button.dataset
                        .lightValue ??
                    null;

                if (
                    command ===
                        "brightness" ||
                    command ===
                        "temperature"
                ) {
                    value =
                        Number(value);
                }

                sendLightCommand(
                    command,
                    value
                );
            }
        );
    });

document
    .querySelectorAll(
        "[data-light-color]"
    )
    .forEach(button => {

        button.addEventListener(
            "click",
            () => {
                const rgb =
                    hexToRgb(
                        button.dataset
                            .lightColor
                    );

                lightColor.value =
                    button.dataset
                        .lightColor;

                sendLightCommand(
                    "color",
                    rgb
                );
            }
        );
    });

if (
    lightBrightness &&
    lightBrightnessValue
) {
    lightBrightness.addEventListener(
        "input",
        () => {
            lightBrightnessValue.textContent =
                `${lightBrightness.value}%`;
        }
    );

    lightBrightness.addEventListener(
        "change",
        () => {
            sendLightCommand(
                "brightness",
                Number(
                    lightBrightness.value
                )
            );
        }
    );
}

if (
    setLightColorButton &&
    lightColor
) {
    setLightColorButton.addEventListener(
        "click",
        () => {
            sendLightCommand(
                "color",
                hexToRgb(
                    lightColor.value
                )
            );
        }
    );
}


// ---------------------------------------------------------
// Event handlers / startup
// ---------------------------------------------------------

document
    .querySelectorAll(
        "[data-command]"
    )
    .forEach(button => {
        button.addEventListener(
            "click",
            () => sendCommand(
                button.dataset.command
            )
        );
    });

connectButton.addEventListener(
    "click",
    connectDevice
);

findRemoteButton.addEventListener(
    "click",
    findRemote
);

deviceSelect.addEventListener(
    "change",
    () => selectDevice(
        deviceSelect.value
    )
);

setInterval(
    () => refreshSelectedStatus(),
    5000
);

if (parentalDeviceSelect) {
    parentalDeviceSelect.addEventListener(
        "change",
        renderSelectedParentalRule
    );
}

if (saveParentalControlsButton) {
    saveParentalControlsButton.addEventListener(
        "click",
        saveParentalControls
    );
}

loadDevices();
loadParentalControls();

setInterval(
    loadParentalControls,
    30000
);