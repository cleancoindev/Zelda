blockStartMessage = 0

function start()
  blockStartMessage = blockStartMessage + 1
  if blockStartMessage > 7 then
    blockStartMessage = 1
  end
	
  chatText = "${block_text_" .. tostring(blockStartMessage) .. "}"
  message("<message message_type=\"show_text\" language_string=\"" .. chatText .. "\"/>");
end
