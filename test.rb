200.times do
  Thread.new { `./klient 100 1 1` }
end
Thread.new { `./klient 100 100 #{1..1000.join(" ")}` }
