<?php

// The admin session: a cookie that scripts can't read and other sites can't send
function AdminSessionStart() {
   if (session_status() === PHP_SESSION_ACTIVE) return;
   $https = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off')
      || (($_SERVER['HTTP_X_FORWARDED_PROTO'] ?? '') === 'https');
   ini_set('session.use_strict_mode', '1');
   session_set_cookie_params([
      'lifetime' => 0,
      'path'     => '/',
      'secure'   => $https,
      'httponly' => true,
      'samesite' => 'Strict',
   ]);
   session_start();
}

// text from the XLX directory, which hands out < > & raw or as entities, made safe for HTML
function DirectoryText($s) {
   return htmlspecialchars(html_entity_decode((string)$s, ENT_QUOTES | ENT_HTML5, 'UTF-8'), ENT_QUOTES, 'UTF-8');
}

function GetSystemUptime() {
   $out = exec("uptime");
   return substr($out, 0, strpos($out, ","));
}

function Debug($message) {
   echo '<br><hr><pre>';
   print_r($message);
   echo '</pre><hr><br>';
}

function ParseTime($Input) {
   
    if (strpos($Input, "<") !== false) {
       $Input = substr($Input, 0, strpos($Input, "<"));
    }
    
    if (strpos($Input, "T") !== false) {
        return strtotime($Input);
    }

    // Tuesday Tue Nov 17 14:23:22 2015
    $tmp  = explode(" ", $Input);
    if (strlen(trim($tmp[3])) == 0) {
       unset($tmp[3]);
       $tmp = array_values($tmp);
    }

    $tmp1 = explode(":", $tmp[4]); 
    $month = "";
    switch (strtolower($tmp[2])) {
      case 'jan' : $month = 1; break;
      case 'feb' : $month = 2; break;
      case 'mar' : $month = 3; break;
      case 'apr' : $month = 4; break;
      case 'may' : $month = 5; break;
      case 'jun' : $month = 6; break;
      case 'jul' : $month = 7; break;
      case 'aug' : $month = 8; break;
      case 'sep' : $month = 9; break;
      case 'oct' : $month = 10; break;
      case 'nov' : $month = 11; break;
      case 'dec' : $month = 12; break;
      default    : $month = 1; 
    }
    return @mktime($tmp1[0], $tmp1[1], $tmp1[2], $month, $tmp[3], $tmp[5]);
    
}

function FormatSeconds($seconds) {
  $seconds = abs($seconds); 
  return sprintf("%d days %02d:%02d:%02d", $seconds/60/60/24,($seconds/60/60)%24,($seconds/60)%60,$seconds%60);
} 

// Parse a reflector list payload into an array of <reflector> elements, or false if
// the payload is not usable. A reachable-but-broken server (error page, maintenance
// notice) yields no elements; treat that like an outage rather than letting a later
// count() fatal on a non-array.
function ParseReflectorList($Input) {
	if ($Input === false || $Input === '') return false;

	$XML           = new ParseXML();
	$Reflectorlist = $XML->GetElement($Input, "reflectorlist");
	$Reflectors    = $XML->GetAllElements($Reflectorlist, "reflector");

	return is_array($Reflectors) ? $Reflectors : false;
}

// Fetch the XLX directory reflector list, backed by an on-disk cache. Returns an
// array of <reflector> elements, or false if neither the server nor the cache can
// supply one.
//
// The cache does double duty: a fresh entry serves the page without touching the
// network at all, and a stale one keeps the list working while the directory is
// down. The explicit timeout matters on a miss -- without it PHP waits
// default_socket_timeout on an unreachable host and ties up an fpm worker for the
// whole page load.
function FetchReflectorList($ServerURL, $Timeout = 5, $CacheFile = null, $CacheTTL = 300) {
	if ($CacheFile === null) $CacheFile = sys_get_temp_dir().'/urfd-reflectorlist.cache';

	// Fresh cache: serve it and skip the network entirely.
	if (is_readable($CacheFile) && (time() - @filemtime($CacheFile)) < $CacheTTL) {
		$Cached = ParseReflectorList(@file_get_contents($CacheFile));
		if ($Cached !== false) return $Cached;
	}

	$ctx = @stream_context_create(array('http' => array('method'        => 'GET',
	                                                    'timeout'       => $Timeout,
	                                                    'ignore_errors' => true)));
	$Input      = @file_get_contents($ServerURL."?do=GetReflectorList", false, $ctx);
	$Reflectors = ParseReflectorList($Input);

	if ($Reflectors !== false) {
		// Write via a temporary and rename so a concurrent reader never sees a
		// half-written cache.
		$tmp = $CacheFile.'.'.getmypid().'.tmp';
		if (@file_put_contents($tmp, $Input) !== false) {
			if (!@rename($tmp, $CacheFile)) @unlink($tmp);
		}
		return $Reflectors;
	}

	// Server unusable -- fall back to the cache at any age.
	error_log("Reflector list: no usable response from ".$ServerURL.", trying stale cache");
	if (is_readable($CacheFile)) {
		$Stale = ParseReflectorList(@file_get_contents($CacheFile));
		if ($Stale !== false) return $Stale;
	}
	return false;
}

// XLX directory settings for the reflector list. Configs from before the
// reflector took over CallingHome have them in $CallingHome, with the cache
// beside the hash file. The current template no longer sets $CallingHome, so
// if it is there it comes from the operator and wins over the template's
// $Directory defaults (e.g. an old ../config.inc.php override).
function DirectoryConfig() {
	global $Directory, $CallingHome;
	$new = (isset($Directory) && is_array($Directory)) ? $Directory : array();
	$old = (isset($CallingHome) && is_array($CallingHome)) ? $CallingHome : array();
	return array(
		'ServerURL' => $old['ServerURL'] ?? $new['ServerURL'] ?? 'http://xlxapi.rlx.lu/api.php',
		'Timeout'   => (int)($old['Timeout'] ?? $new['Timeout'] ?? 5),
		'CacheDir'  => isset($old['HashFile']) ? dirname($old['HashFile']) : ($new['CacheDir'] ?? '/var/lib/xlxd-ch'),
	);
}

// Convenience wrapper for the pages.
function GetReflectorList() {
	$c = DirectoryConfig();
	return FetchReflectorList($c['ServerURL'], $c['Timeout'], $c['CacheDir'].'/reflectorlist.cache');
}

// Inline notice for pages whose remote data source is unavailable.
function DirectoryUnavailableNotice() {
	return '<div class="note"><p>The XLX directory server cannot be reached right now, '
	     . 'so this list cannot be shown. The reflector is not affected and keeps running.</p>'
	     . '<p class="muted">The list comes back on its own once the directory answers again.</p></div>';
}

// text for the page, escaped
function h($s) {
	return htmlspecialchars((string)$s, ENT_QUOTES, 'UTF-8');
}

// "just now", "4 min ago", "2 h 5 min ago", "3 d ago"
function FormatAgo($time) {
	$s = time() - (int)$time;
	if ($s < 60) return 'just now';
	$m = intdiv($s, 60);
	if ($m < 60) return $m.' min ago';
	if ($m < 1440) return intdiv($m, 60).' h '.($m % 60).' min ago';
	return intdiv($m, 1440).' d ago';
}

// a duration: "45 min", "3 h 12 min", "4 d 2 h"
function FormatSpan($seconds) {
	$m = intdiv(abs((int)$seconds), 60);
	$d = intdiv($m, 1440);
	$hr = intdiv($m % 1440, 60);
	if ($d > 0) return $d.' d '.$hr.' h';
	if ($hr > 0) return $hr.' h '.($m % 60).' min';
	return ($m % 60).' min';
}

// an IP shown as far as the page options allow
function MaskIP($ip, $mode, $mc) {
	if ($mode == 'HideIP') return '';
	$bytes = explode('.', $ip);
	if (count($bytes) == 4) {
		switch ($mode) {
			case 'ShowLast1ByteOfIP': return $mc.'.'.$mc.'.'.$mc.'.'.$bytes[3];
			case 'ShowLast2ByteOfIP': return $mc.'.'.$mc.'.'.$bytes[2].'.'.$bytes[3];
			case 'ShowLast3ByteOfIP': return $mc.'.'.$bytes[1].'.'.$bytes[2].'.'.$bytes[3];
			default:                  return $ip;
		}
	}
	// IPv6: the last two groups
	$count = substr_count($ip, ':');
	if ($count > 1) {
		if ($mode == 'ShowFullIP') return $ip;
		if (substr_count($ip, '::') == 1) $ip = str_replace('::', str_repeat(':', 9 - $count), $ip);
		$groups = explode(':', $ip);
		if (count($groups) == 8) {
			$out = implode(':', array_fill(0, 6, $mc));
			for ($k = 6; $k < 8; $k++) $out .= ':'.(($groups[$k] === '') ? '0' : $groups[$k]);
			return $out;
		}
	}
	return '';
}

// the configured modules from the status file, keyed by letter
function DashboardModules($Reflector) {
	global $PageOptions;
	$Modules = array();
	$content = $Reflector->GetXMLContent();
	if ($content !== null) {
		$XML = new ParseXML();
		$block = $XML->GetElement($content, 'Modules');
		$items = $block ? $XML->GetAllElements($block, 'Module') : array();
		foreach ($items as $item) {
			$name = trim((string)$XML->GetText($item, 'Name'));
			if ($name == '') continue;
			$mappings = array();
			foreach ($XML->GetAllElements($item, 'Mapping') as $map) {
				$mappings[] = array(
					'protocol'   => (string)$XML->GetText($map, 'Protocol'),
					'type'       => (string)$XML->GetText($map, 'Type'),
					'id'         => (string)$XML->GetText($map, 'ID'),
					'timeslot'   => (string)$XML->GetText($map, 'Timeslot'),
					'remoteName' => (string)$XML->GetText($map, 'RemoteName'),
					'dynamic'    => ($XML->GetText($map, 'Dynamic') === 'true'),
					'remaining'  => (int)$XML->GetText($map, 'Remaining'),
				);
			}
			$Modules[$name] = array(
				'name'       => trim((string)$XML->GetText($item, 'Description')),
				'nodes'      => (int)$XML->GetText($item, 'LinkedNodes'),
				'onair'      => trim((string)$XML->GetText($item, 'OnAir')),
				'echo'       => ($XML->GetText($item, 'Echo') === 'true'),
				'transcoded' => ($XML->GetText($item, 'Transcoded') === 'true'),
				'mappings'   => $mappings,
				'dmrplus'    => (string)$XML->GetText($item, 'DMRplus'),
				'ysfdgid'    => (string)$XML->GetText($item, 'YSFDGID'),
				'nxdnran'    => (string)$XML->GetText($item, 'NXDNRAN'),
				'nxdndstid'  => (string)$XML->GetText($item, 'NXDNDstId'),
			);
		}
	}
	// module names from an older config, for modules the reflector does not describe
	if (isset($PageOptions['ModuleNames']) && is_array($PageOptions['ModuleNames'])) {
		foreach ($PageOptions['ModuleNames'] as $mod => $name) {
			if (isset($Modules[$mod]) && $Modules[$mod]['name'] == '') $Modules[$mod]['name'] = $name;
		}
	}
	ksort($Modules);
	return $Modules;
}

// the protocols the reflector has enabled, as name and port
function DashboardProtocols($Reflector) {
	$out = array();
	$content = $Reflector->GetXMLContent();
	if ($content === null) return $out;
	$XML = new ParseXML();
	$block = $XML->GetElement($content, 'Protocols');
	$items = $block ? $XML->GetAllElements($block, 'Protocol') : array();
	foreach ($items as $item) {
		$name = trim((string)$XML->GetText($item, 'Name'));
		if ($name != '') $out[] = array('name' => $name, 'port' => trim((string)$XML->GetText($item, 'Port')), 'description' => trim((string)$XML->GetText($item, 'Description')));
	}
	return $out;
}

// how a station reached us: its node, and the peer when it came in from another reflector
function StationVia($Reflector, $st) {
    // the log string pads the module with spaces: "DL4JC  S" is DL4JC-S
    $via = preg_replace('/\s+/', '-', trim($st->GetVia()));
    $peer = trim($st->GetPeer());
    $refl = trim((string)$Reflector->GetReflectorName());
    if ($peer != '' && $peer != $refl && $peer != str_replace("XLX", "URF", $refl)) $via .= ($via != '' ? ' / ' : '').$peer;
    return $via;
}

// everyone on the air, one array(module, callsign, station or null) per module with an
// open stream, the transmission that started last first
function OnAirAll($Reflector, $Modules) {
	$out = array();
	foreach ($Modules as $mod => $info) {
		if ($info['onair'] == '') continue;
		$cs = trim($info['onair']);
		$found = null;
		for ($i = 0; $i < $Reflector->StationCount(); $i++) {
			$st = $Reflector->Stations[$i];
			if ($st->GetModule() == $mod && trim($st->GetCallsignOnly()) == $cs) { $found = $st; break; }
		}
		$out[] = array($mod, $cs, $found);
	}
	usort($out, function ($a, $b) {
		return ($b[2] ? $b[2]->GetLastHeardTime() : 0) <=> ($a[2] ? $a[2]->GetLastHeardTime() : 0);
	});
	return $out;
}

// the "also on" box for the other modules on the air: three letters at most, the rest
// as a count, all of them in the tooltip
function AlsoOnAir($mods) {
	if (count($mods) == 0) return '';
	$shown = array_slice($mods, 0, 3);
	$more = count($mods) - count($shown);
	return '<span class="lcd-also" title="Also on the air: module '.h(implode(', ', $mods)).'">also on '
		.h(implode(', ', $shown)).($more > 0 ? ' +'.$more : '').'</span>';
}

// the transmission that started last, or null when nobody transmits
function OnAirNow($Reflector, $Modules) {
	$all = OnAirAll($Reflector, $Modules);
	return count($all) ? $all[0] : null;
}

// a fingerprint of the status file: who is on the air on which module and who was heard
// last; it changes on every speaker change, so the pages reload right then
function OnAirFingerprint($xml) {
	$parts = array();
	preg_match_all('#<Module>(.*?)</Module>#s', (string)$xml, $modules);
	foreach ($modules[1] as $mod) {
		if (preg_match('#<Name>([A-Z])</Name>#', $mod, $n) && preg_match('#<OnAir>([^<]*)</OnAir>#', $mod, $o)) {
			$parts[] = $n[1].'='.trim($o[1]);
		}
	}
	if (preg_match('#<STATION>.*?<Callsign>([^<]*)</Callsign>.*?<LastHeardTime>([^<]*)</LastHeardTime>#s', (string)$xml, $st)) {
		$parts[] = trim($st[1]).'@'.trim($st[2]);
	}
	return md5(implode('|', $parts));
}

function CreateCode ($laenge) {
	$zeichen = "1234567890abcdefghijklmnopqrstuvwyxzABCDEFGHIJKLMNAOPQRSTUVWYXZ";   
	mt_srand( (double) microtime() * 1000000); 
	$out = "";
	for ($i=1;$i<=$laenge;$i++){ 
		$out .= $zeichen[mt_rand(0,(strlen($zeichen)-1))];       
	}         
	return $out;  
}

?>