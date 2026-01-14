# ESP32 RFID Door Lock System - Firebase Setup Guide

## Quick Start

### Step 1: Create Firebase Project

1. Go to [Firebase Console](https://console.firebase.google.com/)
2. Click "Add Project"
3. Enter project name (e.g., "rfid-door-lock")
4. Disable Google Analytics (optional for this project)
5. Click "Create Project"

### Step 2: Enable Realtime Database

1. In Firebase Console, go to **Build > Realtime Database**
2. Click "Create Database"
3. Choose your region (closest to you)
4. Start in **Test Mode** (we'll secure it later)
5. Click "Enable"

### Step 3: Enable Authentication

1. Go to **Build > Authentication**
2. Click "Get Started"
3. Go to **Sign-in method** tab
4. Enable **Email/Password**
5. Also enable **Anonymous** (for dashboard testing)

### Step 4: Create Device User

1. In Authentication, go to **Users** tab
2. Click "Add user"
3. Email: `esp32device@yourdomain.com` (or any email)
4. Password: Choose a strong password
5. Save these credentials for the ESP32 code

### Step 5: Get Firebase Configuration

#### For ESP32 (Arduino):
1. Go to **Project Settings** (gear icon)
2. Under "General" tab, find **Web API Key** - this is your `API_KEY`
3. Go to **Realtime Database** and copy the URL - this is your `DATABASE_URL`

#### For Web Dashboard:
1. Go to **Project Settings**
2. Scroll to "Your apps" and click **</>** (Web)
3. Register app (any nickname)
4. Copy the `firebaseConfig` object

### Step 6: Update Your Code

#### ESP32 Code (working.ino):
```cpp
#define API_KEY "AIzaSy..."                                    // From Project Settings
#define DATABASE_URL "https://your-project.firebaseio.com"     // From Realtime Database
#define USER_EMAIL "esp32device@yourdomain.com"                // User you created
#define USER_PASSWORD "your_password"                          // Password you set
```

#### Dashboard (index.html):
```javascript
const firebaseConfig = {
    apiKey: "AIzaSy...",
    authDomain: "your-project.firebaseapp.com",
    databaseURL: "https://your-project.firebaseio.com",
    projectId: "your-project",
    storageBucket: "your-project.appspot.com",
    messagingSenderId: "123456789",
    appId: "1:123456789:web:abc123"
};
```

### Step 7: Set Database Rules (Security)

Go to **Realtime Database > Rules** and paste:

```json
{
  "rules": {
    "devices": {
      "$deviceId": {
        // Only authenticated users can read/write
        ".read": "auth != null",
        ".write": "auth != null",
        
        "whitelist": {
          ".indexOn": ["name"]
        },
        "blacklist": {
          ".indexOn": ["name"]
        },
        "pending": {
          ".indexOn": ["firstSeen"]
        },
        "logs": {
          ".indexOn": [".key", "time", "status"]
        }
      }
    }
  }
}
```

For development/testing, you can use more permissive rules:
```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```

---

## Firebase Database Structure

```
/devices
  /device1
    /whitelist
      /ABCD1234
        name: "John Doe"
        addedAt: "2026-01-14T10:30:00Z"
    /blacklist
      /EFGH5678
        name: "Banned User"
        addedAt: "2026-01-14T10:30:00Z"
    /pending
      /IJKL9012
        name: "Unknown"
        firstSeen: "2026-01-14T10:30:00Z"
    /logs
      /1705234200
        uid: "ABCD1234"
        name: "John Doe"
        status: "granted"
        type: "rfid"
        time: "2026-01-14T10:30:00Z"
        device: "device1"
    /commands
      /unlock
        duration: 5
        timestamp: 1705234200000
    /status
      online: true
      lastSeen: "2026-01-14T10:30:00Z"
      ip: "192.168.1.100"
      rssi: -45
      freeHeap: 234567
      uptime: 3600
```

---

## Arduino Library Installation

In Arduino IDE, install these libraries:

1. **Firebase ESP Client** by Mobizt
   - Sketch > Include Library > Manage Libraries
   - Search "Firebase ESP Client"
   - Install the one by "Mobizt"

2. **ArduinoJson** by Benoit Blanchon (if not already installed)

3. **MFRC522** by GithubCommunity

4. **NTPClient** by Fabrice Weinberg

---

## Testing

### Test ESP32 Connection:
1. Upload the code to ESP32
2. Open Serial Monitor (115200 baud)
3. You should see:
   ```
   === ESP32 RFID Door Lock System with Firebase ===
   RFID Reader initialized
   LittleFS mounted successfully
   WiFi connected!
   Firebase connected!
   Device status updated
   ```

### Test Dashboard:
1. Open `index.html` in a web browser
2. You should see the device status as "Online"
3. Try scanning a card - it should appear in "Pending"
4. Approve the card - it moves to "Whitelist"
5. Scan again - access should be granted

### Test Remote Unlock:
1. Click "Remote Unlock" on dashboard
2. Enter duration (e.g., 5 seconds)
3. Click Unlock
4. ESP32 should activate relay

---

## Troubleshooting

### ESP32 Issues:

**"Firebase not ready":**
- Check API_KEY and DATABASE_URL
- Verify WiFi connection
- Check user email/password

**"Token error":**
- Make sure Email/Password auth is enabled
- Verify user credentials are correct

**"Stream error":**
- Check database rules allow reading
- Verify DATABASE_URL is correct

### Dashboard Issues:

**"Permission denied":**
- Check database rules
- Verify authentication is enabled
- Try enabling Anonymous auth

**"Cannot connect":**
- Verify firebaseConfig values
- Check browser console for errors
- Ensure CORS is not blocking requests

---

## Multiple Devices

To add more ESP32 devices:

1. Change `device_id` in the ESP32 code:
   ```cpp
   const char* device_id = "device2";
   ```

2. Create a new device user in Firebase Auth (optional - can share credentials)

3. Update dashboard to select device:
   ```javascript
   const DEVICE_ID = "device2"; // or make this selectable
   ```

---

## Cost Considerations

Firebase Free Tier includes:
- **Realtime Database**: 1GB storage, 10GB/month download
- **Authentication**: Unlimited users

For a door lock system with ~100 scans/day:
- ~6KB/day logs = ~180KB/month
- Well within free tier limits

---

## Security Best Practices

1. **Use strong passwords** for device authentication
2. **Set proper database rules** - don't leave in test mode
3. **Use HTTPS** for dashboard hosting
4. **Regularly audit** whitelist/blacklist
5. **Monitor logs** for suspicious activity
6. **Enable 2FA** on your Google account

---

## Support

For issues:
1. Check Serial Monitor output
2. Check Firebase Console > Realtime Database for data
3. Check browser Developer Tools > Console for errors
