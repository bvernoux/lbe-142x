import serial

def parse_rmc(fields):
    return {
        "type": "RMC",
        "utc": fields[1],
        "status": "valid" if fields[2] == "A" else "no_fix",
        "lat": fields[3] + " " + fields[4] if fields[3] else None,
        "lon": fields[5] + " " + fields[6] if fields[5] else None,
        "speed_knots": fields[7] or None,
        "course": fields[8] or None,
        "date": fields[9] or None,
    }

def parse_gga(fields):
    return {
        "type": "GGA",
        "utc": fields[1],
        "lat": fields[2] + " " + fields[3] if fields[2] else None,
        "lon": fields[4] + " " + fields[5] if fields[4] else None,
        "fix_quality": fields[6],
        "sats_used": fields[7],
        "hdop": fields[8],
        "altitude_m": fields[9] or None,
    }

def parse_gsa(fields):
    used_prns = [f for f in fields[3:15] if f]
    return {
        "type": "GSA",
        "mode": fields[1],
        "fix_type": fields[2],   # 1=no fix, 2=2D, 3=3D
        "used_prns": used_prns,
        "pdop": fields[15] if len(fields) > 15 else None,
        "hdop": fields[16] if len(fields) > 16 else None,
        "vdop": fields[17].split("*")[0] if len(fields) > 17 else None,
    }

def parse_gsv(fields):
    sats_in_view = fields[3]
    sats = []
    for i in range(4, len(fields) - 3, 4):
        prn = fields[i]
        elev = fields[i+1] if i+1 < len(fields) else ""
        az = fields[i+2] if i+2 < len(fields) else ""
        snr = fields[i+3].split("*")[0] if i+3 < len(fields) else ""
        if prn:
            sats.append({
                "prn": prn,
                "elevation": elev,
                "azimuth": az,
                "snr": snr or None
            })
    return {
        "type": "GSV",
        "msg_num": fields[2],
        "total_msgs": fields[1],
        "sats_in_view": sats_in_view,
        "satellites": sats,
    }

def parse_nmea(line):
    if not line.startswith("$"):
        return None

    fields = line.strip().split(",")
    sentence = fields[0][3:]  # e.g. RMC, GGA, GSA, GSV

    if sentence == "RMC":
        return parse_rmc(fields)
    elif sentence == "GGA":
        return parse_gga(fields)
    elif sentence == "GSA":
        return parse_gsa(fields)
    elif sentence == "GSV":
        return parse_gsv(fields)
    return None

ser = serial.Serial("/dev/ttyACM0", 9600, timeout=1)

while True:
    line = ser.readline().decode(errors="ignore").strip()
    if not line:
        continue

    #print("RAW:", line)
    parsed = parse_nmea(line)
    if parsed:
        print("PARSED:", parsed)

#import serial

#ser = serial.Serial('/dev/ttyACM0', 9600, timeout=1)   # Linux example
# On Windows use something like 'COM5'

#while True:
 #   line = ser.readline().decode(errors='ignore').strip()
  #  if line:
   #     print(line)
